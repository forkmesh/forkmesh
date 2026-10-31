import json
import re


EXPORT_PATH = "/api/accounts/export"
PREFIX = "/api/accounts/data"
EXPORT_SCHEMA = "forkmesh-account-export-v1"
INVENTORY_SCHEMA = "forkmesh-account-data-v1"
MAX_BODY_BYTES = 320 * 1024
MAX_VALUE_BYTES = 256 * 1024
MAX_ACCOUNT_BYTES = 4 * 1024 * 1024
MAX_ITEMS = 200
MAX_LABEL = 120
MAX_DEVICE = 80
PAGE_ROWS = 50
SCAN_CAP = 5000
SCAN_BATCH = 40
SCAN_BATCH_PARAMS = 400
EXPORT_ROWS_PER_TABLE = 2000
MAX_FIELD_CHARS = 8192
INVENTORY_TTL_MS = 60 * 1000
EXPORT_INTERVAL_MS = 30 * 1000
MEMO_MAX = 256
KEY_RE = re.compile(r"^[a-z0-9][a-z0-9._-]{0,79}$")
TABLE_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{0,63}$")
SEALED_RE = re.compile(r"^[A-Za-z0-9+/]{40,}={0,2}$")
KINDS = ("layout", "subscriptions", "settings", "app")
MANAGER_ROLES = frozenset({"owner", "admin"})
STORE_TABLE = "account_store"
STORE_KEYS_TABLE = "account_store_keys"
STORE_SCHEMA = (
    """CREATE TABLE IF NOT EXISTS account_store (
        account_bi TEXT NOT NULL,
        item_key TEXT NOT NULL,
        kind TEXT NOT NULL DEFAULT 'settings',
        data TEXT NOT NULL,
        bytes INTEGER NOT NULL DEFAULT 0 CHECK(bytes >= 0),
        revision INTEGER NOT NULL DEFAULT 1 CHECK(revision >= 1),
        updated_at INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (account_bi, item_key))""",
    """CREATE TABLE IF NOT EXISTS account_store_keys (
        account_bi TEXT PRIMARY KEY,
        wrapped TEXT NOT NULL,
        created_at INTEGER NOT NULL DEFAULT 0)""",
)
PLAIN_COLUMNS = frozenset({
    "account", "account_name", "actor", "added_by", "approved_by",
    "attendee_name", "captain_name", "challenger_name", "created_by",
    "follower_name", "fork_owner", "granted_by", "installed_by_name",
    "lifted_by", "node", "node_name", "node_owner", "owner",
    "owner_approved_by_name", "owner_name", "paid_by_name", "principal_name",
    "pushed_by", "recipient_name", "referred_name", "referrer_name",
    "registered_by", "repo_owner", "requested_by", "reviewed_by",
    "reviewer_name", "signer_account", "submitted_by_name", "successor_name",
    "target_name", "target_node", "updated_by", "username",
})
ACTOR_COLUMNS = frozenset({
    "added_by_bi", "approved_by_bi", "changed_by_bi", "completed_by_bi",
    "created_by_bi", "decided_by_bi", "deleted_by_bi", "granted_by_bi",
    "installed_by_bi", "invited_by_bi", "last_used_by_bi",
    "owner_approved_by_bi", "recorded_by_bi", "reviewed_by_bi",
    "updated_by_bi", "verified_by_bi",
})
CREATOR_COLUMNS = frozenset({
    "added_by_bi", "created_by_bi", "installed_by_bi", "invited_by_bi",
})
TOUCH_COLUMNS = ACTOR_COLUMNS - CREATOR_COLUMNS
SKIPPED_TABLES = frozenset({STORE_TABLE, STORE_KEYS_TABLE, "schema_meta", "d1_migrations"})
WITHHELD_TABLES = frozenset({
    "admin_signup_referral_code", "app_auth_codes", "apns_device_tokens",
    "app_tokens", "bounty_wallet", "chain_intents", "chat_channel_webhooks",
    "link_codes", "org_bot_tokens", "password_reset_rate",
    "pending_verifications",
    "patreon_oauth_states", "quarantined_nodes", "quarantined_users",
    "repo_action_secrets", "runner_setup_tokens", "security_reports",
    "sensitive_audit_log", "workflow_credentials",
})
OPEN_KEY_TABLES = frozenset({"account_ssh_keys", "owner_encryption_keys"})
STAFF_NOTE_TABLES = frozenset({"moderation_blocks", "repo_terms_flags"})
DELETABLE = {
    "notifications": "recipient_bi",
    "repo_stars": "account_bi",
    "profile_follows": "follower_bi",
    "comment_reactions": "account_bi",
    "thread_subscriptions": "subscriber_bi",
    "chat_read_marks": "account_bi",
    "chat_channel_reads": "account_bi",
    "recent_pushes": "account_bi",
    "world_user_activity": "account_bi",
    "account_activity_days": "account_bi",
    "world_lobby_links": "account_bi",
    "world_floor_note_replies": "account_bi",
    "world_inactive_presence": "account_bi",
    "world_office_attendance": "account_bi",
    "account_presence": "name_bi",
    "member_repo_activity": "user_bi",
    "marketplace_package_uses": "account_bi",
    "marketplace_package_activity": "account_bi",
    "feedback_email_sends": "account_bi",
    "login_attempts": "id_bi",
}
TABLE_LABELS = {
    "users": "Your account record",
    "nodes": "Nodes linked to your account",
    "account_devices": "Signed-in devices",
    "account_sessions": "Sign-in sessions",
    "account_ssh_keys": "SSH keys",
    "account_presence": "Online presence",
    "account_activity_days": "Days you were active",
    "account_package_installs": "Installed packages and their settings",
    "apns_device_tokens": "Push notification devices",
    "notifications": "Notifications",
    "repositories": "Repositories",
    "hosted_repos": "Hosted repositories",
    "repo_stars": "Stars you gave",
    "repo_forks": "Forks",
    "profile_follows": "Follows",
    "comment_reactions": "Reactions",
    "comment_tips": "Tips",
    "thread_subscriptions": "Followed threads",
    "chat_read_marks": "Chat read positions",
    "chat_channel_reads": "Channel read positions",
    "chat_channel_members": "Chat channel memberships",
    "chat_direct_participants": "Direct conversations",
    "chat_direct_media": "Direct message attachments",
    "recent_pushes": "Recent pushes",
    "org_members": "Organization memberships",
    "org_team_members": "Team memberships",
    "orgs": "Organization record",
    "org_repos": "Organization repositories",
    "notes": "Notes",
    "note_shares": "Note shares",
    "note_versions": "Note versions",
    "calendar_events": "Calendar events",
    "calendar_attendees": "Calendar invitations",
    "organization_tasks": "Tasks",
    "badge_awards": "Badges",
    "badge_progress": "Badge progress",
    "referral_attributions": "Referrals",
    "referral_stats": "Referral totals",
    "world_user_activity": "World activity",
    "world_floor_notes": "World floor notes",
    "world_floor_note_replies": "World floor note replies",
    "world_lobby_links": "World lobby links",
    "marketplace_reviews": "Market reviews",
    "marketplace_package_uses": "Package use",
    "marketplace_package_activity": "Package activity",
    "mail_sends": "Emails sent to you",
    "email_sends": "Emails sent to you",
    "login_attempts": "Sign-in attempts",
    "member_repo_activity": "Repository activity",
    "role_grants": "Roles",
    "plan_trials": "Plan trials",
    "patreon_account_links": "Patreon link",
    "workflows": "Workflows",
    "workflow_runs": "Workflow runs",
    "workflow_credentials": "Workflow credentials",
}
CATEGORY_PREFIXES = (
    ("users", "Account"), ("nodes", "Account"), ("account_", "Account"),
    ("login_", "Account"), ("password_", "Account"), ("apns_", "Account"),
    ("owner_encryption", "Account"), ("plan_", "Account"),
    ("notification", "Notifications"), ("thread_", "Notifications"),
    ("chat_", "Chat"), ("org_", "Organizations"),
    ("organization_", "Organizations"), ("orgs", "Organizations"),
    ("repo", "Repositories"), ("hosted_", "Repositories"),
    ("pull_", "Repositories"), ("issue_", "Repositories"),
    ("branch_", "Repositories"), ("commit_", "Repositories"),
    ("release_", "Repositories"), ("security_", "Repositories"),
    ("note", "Notes"), ("calendar_", "Calendar"), ("world_", "World"),
    ("marketplace_", "Market"), ("market_", "Market"), ("app_", "Apps"),
    ("apps", "Apps"), ("badge_", "Badges"), ("referral_", "Referrals"),
    ("hackathon_", "Hackathon"), ("mail_", "Email"), ("email_", "Email"),
    ("marketing_email", "Email"), ("ap_", "Fediverse"),
    ("workflow", "Workflows"), ("runner_", "Runners"),
    ("comment_", "Comments"), ("profile_", "Profile"),
    ("patreon_", "Payments"), ("tip_", "Payments"), ("bounty_", "Payments"),
    ("contributor_", "Contributions"), ("microjob", "Contributions"),
    ("calendar", "Calendar"), ("ad_network", "Ads"), ("community_ad", "Ads"),
)
SECRET_COLUMN_RE = re.compile(
    r"(secret|password|passwd|private|seed|mnemonic|token|api_?key|"
    r"digest|_hash$|^hash$|wrapped|cookie|totp)", re.I)
SECRET_KEY_RE = re.compile(
    r"(secret|password|passwd|pass_?hash|pass_?salt|private|seed|mnemonic|"
    r"token|api_?key|totp|recovery|wrapped|cookie|^key$|_key$|[a-z]Key$|"
    r"session)", re.I)
SAFE_KEY_RE = re.compile(
    r"(public|^pub|key_?id|key_?type|key_?name|fingerprint|^item_?key$|"
    r"^sessionCount$)", re.I)
HASHED = "(hashed identifier)"
WITHHELD = "(withheld: security credential)"
SEALED = "(encrypted with a key only its package or your devices hold)"
ORG_ONLY = "(belongs to the organization; ask an owner or admin)"
OTHERS = "(belongs to someone else; you only acted on it)"
STAFF_NOTE = "(written by a moderator; the note and its author stay with the moderators)"

_INVENTORY_MEMO = {}
_EXPORT_MEMO = {}
_SCHEMA_READY = {"done": False}
_TABLES_MEMO = {"at": 0, "value": None}
TABLES_TTL_MS = 5 * 60 * 1000


def _response(runtime, payload, status=200, filename=""):
    headers = {"x-content-type-options": "nosniff"}
    if filename:
        headers["content-disposition"] = 'attachment; filename="' + filename + '"'
    return runtime.response(
        payload, status=status,
        cache_control="no-store, max-age=0, must-revalidate",
        extra_headers=headers)


def _error(runtime, code, status):
    return _response(runtime, {"ok": False, "error": code}, status)


def _qi(name):
    return '"' + str(name).replace('"', '""') + '"'


def _memo_put(memo, key, value):
    if len(memo) >= MEMO_MAX:
        memo.clear()
    memo[key] = value


def forget(account_bi):
    _INVENTORY_MEMO.pop(str(account_bi or ""), None)


def humanize(table):
    words = str(table).replace("_", " ").strip()
    return (words[:1].upper() + words[1:]) if words else ""


def label(table):
    return TABLE_LABELS.get(table) or humanize(table)


def category(table):
    for prefix, name in CATEGORY_PREFIXES:
        if table.startswith(prefix):
            return name
    return "Other"


def withheld(table):
    return table in WITHHELD_TABLES or (
        table.endswith("_keys") and table not in OPEN_KEY_TABLES)


def identity_columns(columns):
    bis = [c for c in columns if c.endswith("_bi")]
    plain = [c for c in columns if c in PLAIN_COLUMNS]
    return bis, plain


async def ensure_store(runtime):
    if _SCHEMA_READY["done"]:
        return
    for statement in STORE_SCHEMA:
        await runtime.d1_run(statement)
    _SCHEMA_READY["done"] = True


async def tables(runtime):
    now = runtime.now()
    cached = _TABLES_MEMO["value"]
    if cached is not None and now - _TABLES_MEMO["at"] < TABLES_TTL_MS:
        return cached
    master = await runtime.d1_all(
        "SELECT name, sql FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '_cf_%' ORDER BY name")
    result = {}
    columns = {}
    try:
        rows = await runtime.d1_all(
            "SELECT m.name AS t, p.name AS c FROM sqlite_master m, "
            "pragma_table_info(m.name) p WHERE m.type='table' "
            "AND m.name NOT LIKE 'sqlite_%' AND m.name NOT LIKE '_cf_%' "
            "ORDER BY m.name, p.cid")
        for row in rows or []:
            columns.setdefault(str(row.get("t") or ""), []).append(
                str(row.get("c") or ""))
    except Exception:
        columns = {}
    for row in master or []:
        name = str(row.get("name") or "")
        if not TABLE_RE.match(name) or name in SKIPPED_TABLES:
            continue
        names = columns.get(name)
        if names is None:
            info = await runtime.d1_all("PRAGMA table_info(" + _qi(name) + ")")
            names = [str(r.get("name") or "") for r in info or []]
        bis, plain = identity_columns(names)
        if not bis and not plain:
            continue
        rowid = "without rowid" not in " ".join(
            str(row.get("sql") or "").lower().split())
        result[name] = {"columns": names, "bis": bis, "plain": plain,
                        "rowid": rowid}
    _TABLES_MEMO["at"] = now
    _TABLES_MEMO["value"] = result
    return result


async def _actor(runtime, data=None):
    account_bi, record = await runtime.session(data or {})
    if not account_bi or not record:
        return "", {}
    return str(account_bi), record


async def subjects(runtime, account_bi, record):
    name = str(record.get("name") or "").strip().lower()
    bis = [account_bi]
    plain = [name] if name else []
    nodes = []
    try:
        rows = await runtime.d1_all(
            "SELECT node_bi, name FROM nodes WHERE user_bi=?", account_bi)
    except Exception:
        rows = []
    for row in rows or []:
        node_bi = str(row.get("node_bi") or "")
        node = str(row.get("name") or "").strip().lower()
        if node_bi and node_bi not in bis:
            bis.append(node_bi)
        if node and node not in plain:
            plain.append(node)
            nodes.append(node)
    for node in record.get("nodes") or []:
        node = str(node or "").strip().lower()
        if node and node not in plain:
            plain.append(node)
            nodes.append(node)
            bis.append(await runtime.blind(node))
    owners = list(bis)
    email = str(record.get("email") or "").strip().lower()
    if email:
        bis.append(await runtime.blind(email))
    account = {"id": "account", "kind": "account", "label": "Your account",
               "name": name, "nodes": nodes, "readable": True,
               "bis": bis, "plain": plain}
    repo_bis = []
    for table in ("repositories", "hosted_repos"):
        try:
            rows = await runtime.d1_all(
                "SELECT key_bi FROM " + table + " WHERE owner_bi IN "
                "(SELECT value FROM json_each(?))", json.dumps(owners))
        except Exception:
            rows = []
        for row in rows or []:
            key = str(row.get("key_bi") or "")
            if key and key not in repo_bis:
                repo_bis.append(key)
    repos = {"id": "repos", "kind": "repositories",
             "label": "Repositories you own", "readable": True,
             "repositories": len(repo_bis), "bis": repo_bis, "plain": []}
    orgs = []
    try:
        rows = await runtime.d1_all(
            "SELECT m.org_bi AS org_bi, m.role AS role, o.name AS name "
            "FROM org_members m INNER JOIN orgs o ON o.org_bi=m.org_bi "
            "WHERE m.member_bi IN (SELECT value FROM json_each(?)) "
            "ORDER BY o.name", json.dumps(owners))
    except Exception:
        rows = []
    seen = set()
    for row in rows or []:
        org_bi = str(row.get("org_bi") or "")
        org = str(row.get("name") or "").strip().lower()
        if not org_bi or org_bi in seen:
            continue
        seen.add(org_bi)
        role = str(row.get("role") or "member")
        values = [org_bi]
        try:
            linked = await runtime.d1_all(
                "SELECT repo, node_owner FROM org_repos WHERE org_bi=?", org_bi)
        except Exception:
            linked = []
        for item in linked or []:
            repo = str(item.get("repo") or "").strip()
            owner = str(item.get("node_owner") or "").strip().lower()
            if repo and owner:
                values.append(await runtime.blind(owner + "/" + repo))
        orgs.append({"id": "org:" + org, "kind": "organization",
                     "label": org, "name": org, "role": role,
                     "readable": role in MANAGER_ROLES,
                     "bis": values, "plain": []})
    return [account, repos] + orgs


def _public_subject(subject):
    return {key: value for key, value in subject.items()
            if key not in ("bis", "plain")}


def _where(info, subject):
    clauses, params = [], []
    if subject["bis"]:
        values = json.dumps(subject["bis"])
        for column in info["bis"]:
            clauses.append(_qi(column) + " IN (SELECT value FROM json_each(?))")
            params.append(values)
    if subject["plain"]:
        values = json.dumps(subject["plain"])
        for column in info["plain"]:
            clauses.append("lower(" + _qi(column)
                           + ") IN (SELECT value FROM json_each(?))")
            params.append(values)
    return clauses, params


def _matches(row, info, subject):
    bis = set(subject["bis"])
    plain = set(subject["plain"])
    hit = []
    for column in info["bis"]:
        if row.get(column) in bis:
            hit.append(column)
    for column in info["plain"]:
        if str(row.get(column) or "").strip().lower() in plain:
            hit.append(column)
    return hit


def _merged(all_subjects):
    merged = {"bis": [], "plain": []}
    for subject in all_subjects:
        merged["bis"].extend(subject["bis"])
        merged["plain"].extend(subject["plain"])
    return merged


def _count(rows, info, all_subjects):
    truncated = len(rows) > SCAN_CAP
    counts = {}
    for row in rows[:SCAN_CAP]:
        for subject in all_subjects:
            if _matches(row, info, subject):
                counts[subject["id"]] = counts.get(subject["id"], 0) + 1
                break
    return counts, truncated


async def _scan(runtime, table, info, all_subjects):
    clauses, params = _where(info, _merged(all_subjects))
    if not clauses:
        return {}, False
    wanted = info["bis"] + info["plain"]
    try:
        rows = await runtime.d1_all(
            "SELECT " + ",".join(_qi(c) for c in wanted) + " FROM "
            + _qi(table) + " WHERE " + " OR ".join(clauses)
            + " LIMIT " + str(SCAN_CAP + 1), *params)
    except Exception as error:
        if "no such table" in str(error).lower():
            return {}, False
        raise
    return _count(rows or [], info, all_subjects)


async def _scan_batch(runtime, batch, catalog, all_subjects):
    merged = _merged(all_subjects)
    parts, params, scanned = [], [], []
    for table in batch:
        info = catalog[table]
        clauses, values = _where(info, merged)
        if not clauses:
            continue
        wanted = ",".join(_qi(c) for c in info["bis"] + info["plain"])
        parts.append(
            "SELECT * FROM (SELECT ? AS t, json_array(" + wanted + ") AS v FROM "
            + _qi(table) + " WHERE " + " OR ".join(clauses)
            + " LIMIT " + str(SCAN_CAP + 1) + ")")
        params.append(table)
        params.extend(values)
        scanned.append(table)
    if not scanned:
        return {}
    parts.append("SELECT '' AS t, '[]' AS v")
    try:
        rows = await runtime.d1_all(" UNION ALL ".join(parts), *params)
    except Exception:
        return {table: await _scan(runtime, table, catalog[table], all_subjects)
                for table in scanned}
    found = {table: [] for table in scanned}
    for row in rows or []:
        table = str(row.get("t") or "")
        if table not in found:
            continue
        try:
            values = json.loads(row.get("v") or "[]")
        except (TypeError, ValueError):
            continue
        columns = catalog[table]["bis"] + catalog[table]["plain"]
        found[table].append(dict(zip(columns, values)))
    return {table: _count(found[table], catalog[table], all_subjects)
            for table in scanned}


def _batches(tables_sorted, catalog):
    batch, weight = [], 0
    for table in tables_sorted:
        cost = len(catalog[table]["bis"]) + len(catalog[table]["plain"]) + 1
        if batch and (len(batch) >= SCAN_BATCH or weight + cost > SCAN_BATCH_PARAMS):
            yield batch
            batch, weight = [], 0
        batch.append(table)
        weight += cost
    if batch:
        yield batch


async def inventory(runtime, account_bi, record, refresh=False):
    now = runtime.now()
    cached = _INVENTORY_MEMO.get(account_bi)
    if cached and not refresh and now - cached["at"] < INVENTORY_TTL_MS:
        return cached["value"]
    all_subjects = await subjects(runtime, account_bi, record)
    catalog = await tables(runtime)
    results = {}
    for batch in _batches(sorted(catalog), catalog):
        results.update(await _scan_batch(runtime, batch, catalog, all_subjects))
    groups = []
    for table in sorted(results):
        counts, truncated = results[table]
        for subject in all_subjects:
            count = counts.get(subject["id"], 0)
            if not count:
                continue
            groups.append({
                "subject": subject["id"],
                "table": table,
                "label": label(table),
                "category": category(table),
                "rows": count,
                "truncated": truncated,
                "readable": bool(subject["readable"]),
                "withheld": withheld(table),
                "deletable": subject["id"] == "account" and table in DELETABLE,
            })
    value = {"subjects": all_subjects, "groups": groups, "at": now}
    _memo_put(_INVENTORY_MEMO, account_bi, {"at": now, "value": value})
    return value


def _redact(value, depth=0):
    if depth > 12:
        return "(nested too deeply)"
    if isinstance(value, dict):
        cleaned = {}
        for key, item in value.items():
            name = str(key)
            if SECRET_KEY_RE.search(name) and not SAFE_KEY_RE.search(name):
                cleaned[name] = WITHHELD if item not in (None, "", [], {}) else item
            else:
                cleaned[name] = _redact(item, depth + 1)
        return cleaned
    if isinstance(value, list):
        return [_redact(item, depth + 1) for item in value[:1000]]
    return _scalar(value)


def _scalar(value):
    if value is None or isinstance(value, (bool, int, float)):
        return value
    if isinstance(value, (bytes, bytearray)):
        return "(" + str(len(value)) + " bytes of binary data)"
    text = str(value)
    if len(text) > MAX_FIELD_CHARS:
        return text[:MAX_FIELD_CHARS] + "… (" + str(len(text) - MAX_FIELD_CHARS) + " more characters)"
    return text


def _secret_column(column):
    lowered = column.lower()
    if lowered == "data" or lowered.endswith(("_at", "_ts", "_count", "_kind", "_id")):
        return False
    return bool(SECRET_COLUMN_RE.search(lowered))


async def render(runtime, table, info, row, subject, org_managed):
    fields = {}
    hits = _matches(row, info, subject)
    org_restricted = bool(
        subject["id"] == "account" and hits and set(hits) <= ACTOR_COLUMNS
        and "org_bi" in row and row.get("org_bi") not in org_managed)
    touched = bool(subject["id"] == "account" and hits and set(hits) <= TOUCH_COLUMNS)
    for column, value in row.items():
        if column == "_rowid_":
            continue
        if column.endswith("_bi"):
            fields[column] = HASHED if value else value
        elif _secret_column(column):
            fields[column] = WITHHELD if value not in (None, "") else value
        elif isinstance(value, str) and (
                column == "data" or column.endswith(("_data", "sealed"))):
            if org_restricted or touched:
                fields[column] = ORG_ONLY if org_restricted else OTHERS
                continue
            if table in STAFF_NOTE_TABLES:
                fields[column] = STAFF_NOTE if value else value
                continue
            opened = await runtime.open(value) if value else None
            if opened is None:
                fields[column] = SEALED if value else value
            else:
                fields[column] = _redact(opened)
        elif isinstance(value, str) and SEALED_RE.match(value) and len(value) >= 64:
            fields[column] = SEALED
        else:
            fields[column] = _scalar(value)
    item = {"fields": fields}
    if "_rowid_" in row:
        item["rowid"] = int(row["_rowid_"])
    item["deletable"] = bool(
        subject["id"] == "account" and table in DELETABLE and "_rowid_" in row
        and row.get(DELETABLE[table]) in set(subject["bis"]))
    return item


async def _rows(runtime, table, info, subject, after, limit, org_managed):
    clauses, params = _where(info, subject)
    if not clauses:
        return [], 0
    if info["rowid"]:
        sql = ("SELECT rowid AS _rowid_, * FROM " + _qi(table) + " WHERE ("
               + " OR ".join(clauses) + ") AND rowid > ? ORDER BY rowid LIMIT ?")
        params = params + [int(after), int(limit) + 1]
    else:
        sql = ("SELECT * FROM " + _qi(table) + " WHERE " + " OR ".join(clauses)
               + " LIMIT ? OFFSET ?")
        params = params + [int(limit) + 1, int(after)]
    raw = await runtime.d1_all(sql, *params) or []
    more = len(raw) > limit
    raw = raw[:limit]
    items = [await render(runtime, table, info, dict(row), subject, org_managed)
             for row in raw]
    if not more:
        return items, 0
    if info["rowid"]:
        return items, int(raw[-1]["_rowid_"])
    return items, int(after) + limit


def _org_managed(all_subjects):
    return {subject["bis"][0] for subject in all_subjects
            if subject["kind"] == "organization" and subject["readable"]}


def _account_record(record):
    cleaned = _redact({k: v for k, v in record.items() if k != "avatar_png"})
    if record.get("avatar_png"):
        cleaned["avatar_png"] = "(profile picture, " + str(len(str(record["avatar_png"]))) + " characters)"
    return cleaned


async def _store_key(runtime, account_bi, create=False):
    row = await runtime.d1_first(
        "SELECT wrapped FROM account_store_keys WHERE account_bi=?", account_bi)
    if not row and create:
        await runtime.d1_run(
            "INSERT INTO account_store_keys(account_bi,wrapped,created_at) "
            "VALUES(?,?,?) ON CONFLICT(account_bi) DO NOTHING",
            account_bi, await runtime.seal({"key": runtime.new_secret()}),
            runtime.now())
        row = await runtime.d1_first(
            "SELECT wrapped FROM account_store_keys WHERE account_bi=?", account_bi)
    if not row:
        return ""
    opened = await runtime.open(row.get("wrapped") or "")
    return str(opened.get("key") or "") if isinstance(opened, dict) else ""


def _store_meta(row, sealed):
    sealed = sealed if isinstance(sealed, dict) else {}
    return {
        "key": str(row.get("item_key") or ""),
        "kind": str(row.get("kind") or "settings"),
        "label": str(sealed.get("label") or ""),
        "device": str(sealed.get("device") or ""),
        "bytes": int(row.get("bytes") or 0),
        "revision": int(row.get("revision") or 1),
        "updatedAt": int(row.get("updated_at") or 0),
    }


async def store_items(runtime, account_bi, values=False):
    rows = await runtime.d1_all(
        "SELECT item_key,kind,data,bytes,revision,updated_at FROM account_store "
        "WHERE account_bi=? ORDER BY kind,item_key", account_bi)
    if not rows:
        return []
    key = await _store_key(runtime, account_bi)
    items = []
    for row in rows:
        sealed = await runtime.open_with(key, row.get("data") or "") if key else None
        item = _store_meta(row, sealed)
        item["readable"] = isinstance(sealed, dict)
        if values:
            item["value"] = (sealed or {}).get("value") if isinstance(sealed, dict) else None
        items.append(item)
    return items


async def _store_get(runtime, account_bi, item_key):
    row = await runtime.d1_first(
        "SELECT item_key,kind,data,bytes,revision,updated_at FROM account_store "
        "WHERE account_bi=? AND item_key=?", account_bi, item_key)
    if not row:
        return _error(runtime, "not_found", 404)
    key = await _store_key(runtime, account_bi)
    sealed = await runtime.open_with(key, row.get("data") or "") if key else None
    if not isinstance(sealed, dict):
        return _error(runtime, "unreadable", 409)
    item = _store_meta(row, sealed)
    item["value"] = sealed.get("value")
    return _response(runtime, {"ok": True, "item": item})


def _clean_text(value, limit):
    return " ".join(str(value or "").split())[:limit]


async def _store_put(runtime, account_bi, item_key, data):
    if "value" not in data:
        return _error(runtime, "value_required", 400)
    kind = str(data.get("kind") or "settings").strip().lower()
    if kind not in KINDS:
        return _error(runtime, "invalid_kind", 400)
    value = data.get("value")
    encoded = json.dumps(value, separators=(",", ":"))
    size = len(encoded.encode("utf-8"))
    if size > MAX_VALUE_BYTES:
        return _error(runtime, "value_too_large", 413)
    existing = await runtime.d1_first(
        "SELECT revision FROM account_store WHERE account_bi=? AND item_key=?",
        account_bi, item_key)
    base = data.get("baseRevision")
    if base is not None and existing and int(existing.get("revision") or 0) != _int(base):
        return _response(runtime, {"ok": False, "error": "revision_conflict",
                                   "revision": int(existing.get("revision") or 0)}, 409)
    totals = await runtime.d1_first(
        "SELECT COUNT(*) AS n, COALESCE(SUM(bytes),0) AS b FROM account_store "
        "WHERE account_bi=? AND item_key<>?", account_bi, item_key)
    if not existing and int((totals or {}).get("n") or 0) >= MAX_ITEMS:
        return _error(runtime, "too_many_items", 409)
    if int((totals or {}).get("b") or 0) + size > MAX_ACCOUNT_BYTES:
        return _error(runtime, "storage_full", 413)
    key = await _store_key(runtime, account_bi, create=True)
    if not key:
        return _error(runtime, "key_unavailable", 503)
    sealed = await runtime.seal_with(key, {
        "value": value,
        "label": _clean_text(data.get("label"), MAX_LABEL),
        "device": _clean_text(data.get("device"), MAX_DEVICE),
    })
    now = runtime.now()
    revision = int((existing or {}).get("revision") or 0) + 1
    await runtime.d1_run(
        "INSERT INTO account_store(account_bi,item_key,kind,data,bytes,revision,updated_at) "
        "VALUES(?,?,?,?,?,?,?) ON CONFLICT(account_bi,item_key) DO UPDATE SET "
        "kind=excluded.kind,data=excluded.data,bytes=excluded.bytes,"
        "revision=excluded.revision,updated_at=excluded.updated_at",
        account_bi, item_key, kind, sealed, size, revision, now)
    return _response(runtime, {"ok": True, "key": item_key, "revision": revision,
                               "updatedAt": now, "bytes": size})


def _int(value, default=0):
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


async def _store_delete(runtime, account_bi, item_key):
    if item_key:
        result = await runtime.d1_run(
            "DELETE FROM account_store WHERE account_bi=? AND item_key=?",
            account_bi, item_key)
        if not runtime.changes(result):
            return _error(runtime, "not_found", 404)
        return _response(runtime, {"ok": True, "deleted": 1})
    result = await runtime.d1_run(
        "DELETE FROM account_store WHERE account_bi=?", account_bi)
    await runtime.d1_run(
        "DELETE FROM account_store_keys WHERE account_bi=?", account_bi)
    forget(account_bi)
    return _response(runtime, {"ok": True, "deleted": runtime.changes(result)})


def _summary(value, store):
    subjects_out = []
    for subject in value["subjects"]:
        public = _public_subject(subject)
        mine = [g for g in value["groups"] if g["subject"] == subject["id"]]
        public["tables"] = len(mine)
        public["rows"] = sum(g["rows"] for g in mine)
        subjects_out.append(public)
    return {
        "ok": True,
        "schema": INVENTORY_SCHEMA,
        "generatedAt": value["at"],
        "subjects": subjects_out,
        "groups": value["groups"],
        "store": store,
        "storeEncryption": "per-account key, sealed at rest",
    }


async def _inventory_response(runtime, account_bi, record):
    refresh = runtime.query_params().get("refresh") == "1"
    value = await inventory(runtime, account_bi, record, refresh)
    return _response(runtime, _summary(value, await store_items(runtime, account_bi)))


async def _rows_response(runtime, account_bi, record):
    query = runtime.query_params()
    table = str(query.get("table") or "")
    subject_id = str(query.get("subject") or "account")
    catalog = await tables(runtime)
    info = catalog.get(table)
    if not info:
        return _error(runtime, "unknown_table", 404)
    all_subjects = await subjects(runtime, account_bi, record)
    subject = next((s for s in all_subjects if s["id"] == subject_id), None)
    if subject is None:
        return _error(runtime, "unknown_subject", 404)
    base = {"ok": True, "subject": subject_id, "table": table,
            "label": label(table), "category": category(table)}
    if not subject["readable"]:
        return _response(runtime, dict(base, rows=[], next=0, readable=False,
                                       reason="organization_managers_only"))
    if withheld(table):
        return _response(runtime, dict(base, rows=[], next=0, readable=False,
                                       reason="security_credential"))
    after = max(0, _int(query.get("after"), 0))
    rows, cursor = await _rows(runtime, table, info, subject, after, PAGE_ROWS,
                               _org_managed(all_subjects))
    return _response(runtime, dict(base, rows=rows, next=cursor, readable=True,
                                   deletable=subject_id == "account" and table in DELETABLE))


async def _delete_row(runtime, account_bi, record, table, rowid):
    column = DELETABLE.get(table)
    if not column:
        return _error(runtime, "not_deletable", 403)
    all_subjects = await subjects(runtime, account_bi, record)
    account = all_subjects[0]
    try:
        result = await runtime.d1_run(
            "DELETE FROM " + _qi(table) + " WHERE rowid=? AND " + _qi(column)
            + " IN (SELECT value FROM json_each(?))",
            int(rowid), json.dumps(account["bis"]))
    except Exception as error:
        if "no such table" in str(error).lower():
            return _error(runtime, "not_found", 404)
        raise
    if not runtime.changes(result):
        return _error(runtime, "not_found", 404)
    forget(account_bi)
    return _response(runtime, {"ok": True, "deleted": 1, "table": table})


def _filename(name, now):
    stem = re.sub(r"[^a-z0-9-]", "", str(name or "").lower()) or "account"
    return "forkmesh-" + stem + "-export-" + str(int(now)) + ".json"


async def export(runtime, account_bi, record):
    value = await inventory(runtime, account_bi, record, refresh=True)
    all_subjects = value["subjects"]
    managed = _org_managed(all_subjects)
    catalog = await tables(runtime)
    by_id = {s["id"]: s for s in all_subjects}
    sections = []
    devices, ssh_keys = [], []
    for group in value["groups"]:
        subject = by_id[group["subject"]]
        info = catalog.get(group["table"])
        entry = {"subject": group["subject"], "table": group["table"],
                 "label": group["label"], "category": group["category"],
                 "count": group["rows"]}
        if not subject["readable"]:
            entry["rows"] = []
            entry["note"] = "organization_managers_only"
        elif group["withheld"] or not info:
            entry["rows"] = []
            entry["note"] = "security_credential"
        else:
            rows, cursor = await _rows(runtime, group["table"], info, subject, 0,
                                       EXPORT_ROWS_PER_TABLE, managed)
            entry["rows"] = [row["fields"] for row in rows]
            entry["truncated"] = bool(cursor) or group["truncated"]
            if group["subject"] == "account" and group["table"] == "account_devices":
                devices = entry["rows"]
            if group["subject"] == "account" and group["table"] == "account_ssh_keys":
                ssh_keys = entry["rows"]
        sections.append(entry)
    return {
        "ok": True,
        "schema": EXPORT_SCHEMA,
        "exportedAt": runtime.now(),
        "account": _account_record(record),
        "devices": devices,
        "sshKeys": ssh_keys,
        "store": await store_items(runtime, account_bi, values=True),
        "subjects": [_public_subject(s) for s in all_subjects],
        "sections": sections,
    }


async def _export_response(runtime, account_bi, record):
    now = runtime.now()
    last = _EXPORT_MEMO.get(account_bi, 0)
    if now - last < EXPORT_INTERVAL_MS:
        return _error(runtime, "export_rate_limited", 429)
    _memo_put(_EXPORT_MEMO, account_bi, now)
    payload = await export(runtime, account_bi, record)
    return _response(runtime, payload, filename=_filename(record.get("name"), now))


def route(path):
    clean = str(path or "").rstrip("/")
    if clean == EXPORT_PATH:
        return ("export", "", "")
    if clean == PREFIX:
        return ("inventory", "", "")
    if clean == PREFIX + "/rows":
        return ("rows", "", "")
    match = re.fullmatch(re.escape(PREFIX) + r"/rows/([A-Za-z0-9_]{1,64})/([0-9]{1,19})", clean)
    if match:
        return ("row", match.group(1), match.group(2))
    if clean == PREFIX + "/store":
        return ("store", "", "")
    match = re.fullmatch(re.escape(PREFIX) + r"/store/([^/]{1,80})", clean)
    if match:
        return ("store-item", match.group(1), "")
    return None


async def handle(runtime, path):
    target = route(path)
    if target is None:
        return _error(runtime, "not_found", 404)
    kind, first, second = target
    method = runtime.method()
    allowed = {
        "export": ("GET",), "inventory": ("GET",), "rows": ("GET",),
        "row": ("DELETE",), "store": ("GET", "DELETE"),
        "store-item": ("GET", "PUT", "DELETE"),
    }[kind]
    if method not in allowed:
        return _error(runtime, "method_not_allowed", 405)
    data = {}
    if method == "PUT":
        data, problem = await runtime.json_body(MAX_BODY_BYTES)
        if problem:
            return _error(runtime, problem, 413 if problem == "payload_too_large" else 400)
    account_bi, record = await _actor(runtime, data)
    if not account_bi:
        return _error(runtime, "unauthorized", 401)
    await runtime.ensure_schema()
    await ensure_store(runtime)
    if kind == "export":
        return await _export_response(runtime, account_bi, record)
    if kind == "inventory":
        return await _inventory_response(runtime, account_bi, record)
    if kind == "rows":
        return await _rows_response(runtime, account_bi, record)
    if kind == "row":
        return await _delete_row(runtime, account_bi, record, first, second)
    if kind == "store":
        if method == "GET":
            return _response(runtime, {"ok": True, "items": await store_items(runtime, account_bi)})
        return await _store_delete(runtime, account_bi, "")
    if not KEY_RE.match(first):
        return _error(runtime, "invalid_key", 400)
    if method == "GET":
        return await _store_get(runtime, account_bi, first)
    if method == "PUT":
        return await _store_put(runtime, account_bi, first, data)
    return await _store_delete(runtime, account_bi, first)
