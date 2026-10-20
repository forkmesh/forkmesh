"""On-demand implementation of the production administrator console.

Cloudflare validates a Python Worker by compiling and executing its entrypoint
under the isolate memory limit. The console's sizeable HTML/CSS renderer and
generic D1 table tooling are irrelevant to normal public requests, so entry.py
loads this module only when an administrator route needs one of its exports.
"""


def _bind_runtime(runtime):
    """Supply the entrypoint primitives used by the console implementation."""
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


# The console renders inside the shared admin shell (admin_shell.py), which
# owns the palette, sidebar and components. These few rules only cover what
# is particular to the console's own tables: clickable rows, long messages
# that must wrap, and the compact generic-table cells.
CONSOLE_STYLE = """
tr.record-row{cursor:pointer}
tr.record-row:focus td{background:var(--hover)}
tr.record-row:focus{outline:2px solid var(--accent);outline-offset:-2px}
.error-message{display:block;min-width:240px;max-width:520px;white-space:pre-wrap;
  overflow-wrap:anywhere;font-family:ui-monospace,SFMono-Regular,Menlo,
  Consolas,monospace;font-size:12.5px}
.error-users{white-space:nowrap}
.error-anonymous{color:var(--muted)}
table.compact td{max-width:260px;overflow:hidden;text-overflow:ellipsis;
  white-space:nowrap;font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,
  monospace;font-size:12.5px}
table.compact td[title]{cursor:help}
.jsoncell{color:var(--muted)}
th.jcol{font-style:italic;font-weight:500}
td.s5,.s5{color:var(--danger);font-weight:600}
.bulk-bar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;
  padding:12px 18px;border-bottom:1px solid var(--border)}
.bulk-bar .hint{margin:0}
.row-actions{display:flex;gap:6px;justify-content:flex-end;flex-wrap:nowrap}
.row-actions form{margin:0}
.grid.cols-2 .setting{grid-template-columns:minmax(0,1fr)}
.user-grid{display:grid;gap:16px;grid-template-columns:minmax(0,1.6fr)
  minmax(0,1fr);align-items:start}
@media (max-width:1000px){.user-grid{grid-template-columns:minmax(0,1fr)}}
"""

# Copy-to-clipboard for error messages. The console route already allows
# inline script under the global policy; this is the only behaviour the
# server cannot provide. Delegated so every table shares one handler, and
# written to survive non-secure contexts where navigator.clipboard is absent.
CONSOLE_SCRIPT = (
    "<script>"
    "document.addEventListener('click',function(ev){"
    "const btn=ev.target.closest&&ev.target.closest('[data-copy]');"
    "if(!btn)return;ev.preventDefault();ev.stopPropagation();"
    "const text=btn.getAttribute('data-copy')||'';"
    "const label=btn.dataset.copyLabel||btn.textContent;"
    "btn.dataset.copyLabel=label;"
    "function done(ok){btn.textContent=ok?'Copied':'Copy failed';"
    "setTimeout(function(){btn.textContent=label;},1200);}"
    "function fallback(){try{const ta=document.createElement('textarea');"
    "ta.value=text;ta.setAttribute('readonly','');"
    "ta.style.position='fixed';ta.style.opacity='0';"
    "document.body.appendChild(ta);ta.select();"
    "const ok=document.execCommand('copy');ta.remove();done(ok);}"
    "catch(e){done(false);}}"
    "if(navigator.clipboard&&navigator.clipboard.writeText){"
    "navigator.clipboard.writeText(text).then(function(){done(true);},"
    "fallback);}else{fallback();}});"
    "</script>"
)

# Console sections, in sidebar order. The sidebar itself lives in the shell;
# these are the views this module renders. Older bookmarks name a table
# instead of a view, so ``_admin_console_view`` maps those onto a section.
ADMIN_CONSOLE_VIEWS = {
    "overview": "Overview",
    "users": "Users",
    "errors": "Errors",
    "installs": "Install diagnostics",
    "controls": "Controls",
    "database": "Database",
}
ADMIN_TABLE_VIEWS = {
    "users": "users",
    "error_log": "errors",
    "install_diag": "installs",
}
ADMIN_VIEW_TABLES = {view: table for table, view in ADMIN_TABLE_VIEWS.items()}


def _admin_console_view(params):
    """The console section a request asks for (``overview`` by default)."""
    params = params or {}

    def first(key):
        return str((params.get(key) or [""])[0] or "")

    view = first("view")
    if view in ADMIN_CONSOLE_VIEWS:
        return view
    if first("action") in ("detail", "edit", "new"):
        return "database"
    table = first("table")
    if table:
        return ADMIN_TABLE_VIEWS.get(table, "database")
    return "overview"


def _admin_now():
    return int(Date.now())

# Cloudflare D1 internal bookkeeping stays out of the browser. The following
# application tables remain in the inventory but their rows are restricted:
# platform administration is not a decryption capability and the generic UI is
# not a least-privilege security-review interface.
ADMIN_HIDDEN_TABLES = (
    "_cf_KV",
    "accounts",
    "repo_shares",
    "mirror_requests",
    "repo_agents",
    "agent_prompts",
    "owner_encryption_keys",
    "repo_privacy_policy",
    "repo_terms_flags",
    "security_reports",
    "repo_security_scans",
    "repo_security_scan_reviews",
    "role_grants",
    "sensitive_audit_log",
    # Succession is governed only by the organization's current roster. The
    # generic platform-admin table editor must not create approvals, alter
    # cases, or bypass the append-only history/role-transfer guard.
    "org_succession_policies",
    "org_succession_cases",
    "org_succession_approvals",
    "org_succession_events",
    "chain_intents",
    "pending_rewards",
    # Legacy tables can contain Worker-held wallet seeds. They remain frozen
    # for an offline migration, but must never be rendered or edited over HTTP.
    "central_fund",
    "bounty_wallet",
    "issue_bounty",
    "federated_signup",
    # Relay identity is not a Solana wallet, but its private signing seed is
    # likewise outside the generic platform-admin decryption boundary.
    "relay_self",
    # Identity, authorization, mirror-routing and reward state are available
    # only through their purpose-built, audited APIs. A platform administrator
    # is not implicitly a repository owner, mirror signer, or reward signer.
    "account_devices",
    "account_presence",
    "account_ssh_keys",
    "nodes",
    "org_members",
    "org_repos",
    "org_team_members",
    "org_teams",
    "orgs",
    "mirror_https_endpoints",
    "mirror_repository_integrity",
    "private_mirror_routes",
    "reward_contribution_intents",
    "reward_contributions",
    "reward_node_observations",
    "reward_rounds",
    # Microjob claims decide who is owed a payout. Editing one over HTTP would
    # be authoring a transfer, so they move only through their own verified
    # API and the owner-signed payout batcher.
    "microjob_claims",
    "funds_received",
    "clone_rr",
    "edge_route_cursor",
    # ForkMesh Next host state (audit 2026-09-24 H3): membership and mesh
    # routing, the sealed vault, Durable Object storage/alarms, the cron
    # ledger and DNS health votes. An admin write here could re-route mesh
    # traffic, brick unseal or steer DNS, so the generic editor never
    # touches them.
    "cluster_members",
    "host_secrets",
    "do_storage",
    "do_alarms",
    "host_cron_runs",
    "dns_health_votes",
    "dns_reconciler_state",
    # Intrusion detection (docs/IDS.md): signed quarantine records, the
    # incident log and the IDS heartbeats. An admin write could lift or forge
    # a quarantine or erase evidence.
    "cluster_quarantine",
    "security_incidents",
    "ids_heartbeats",
)

# The admin navigation is a live inventory of every application table. Tables
# that hold credentials, encrypted owner data, or audit evidence stay listed so
# an operator can account for the whole schema, but _render_table_view keeps
# their contents restricted.
#
# Row selection + bulk delete (the same "tick rows, Delete selected" UI as the
# error log) is available for every table whose rows are actually rendered —
# i.e. everything not in ADMIN_HIDDEN_TABLES. Sensitive/identity/custody
# tables stay fully restricted above, so there is no separate purge allowlist
# to keep in sync as tables are added.
def _admin_purge_allowed(table):
    return table not in ADMIN_HIDDEN_TABLES


async def _admin_list_tables(env):
    rows = await d1_all(
        env,
        "SELECT name FROM sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%' AND name NOT LIKE '_cf_%' ORDER BY name",
    )
    return [
        str(r.get("name", ""))
        for r in rows
        if (
            r.get("name")
        )
    ]


# Nav row counts. One COUNT(*) round-trip per table (~250 tables) cost ~1.5 s
# of CPU and ~8 s wall per admin page view and tripped the Worker CPU limit, so
# the counts ride in a few compound SELECTs instead.
ADMIN_COUNT_CHUNK = 50


def _admin_count_term(table):
    return "SELECT '%s' AS t, COUNT(*) AS n FROM \"%s\"" % (
        table.replace("'", "''"), table.replace('"', '""'))


async def _admin_table_counts(env, tables):
    counts = {}
    for start in range(0, len(tables), ADMIN_COUNT_CHUNK):
        chunk = tables[start:start + ADMIN_COUNT_CHUNK]
        try:
            rows = await d1_all(
                env, " UNION ALL ".join(_admin_count_term(t) for t in chunk))
            for r in rows:
                counts[str(r.get("t", ""))] = int(r.get("n", 0) or 0)
        except Exception:
            # One unreadable table must not blank the whole chunk.
            for t in chunk:
                try:
                    row = await d1_first(env, _admin_count_term(t))
                    counts[t] = int((row or {}).get("n", 0) or 0)
                except Exception:
                    counts[t] = 0
    for t in tables:
        counts.setdefault(t, 0)
    return counts


# Generic-table cells render on a single compact line (CSS ellipsizes long
# values); anything longer than the preview rides in a hover tooltip so rows
# stay one line tall.
ADMIN_CELL_PREVIEW = 120
ADMIN_CELL_TOOLTIP_MAX = 4000
# Cap on extra columns expanded from the decrypted `data` JSON blob.
ADMIN_MAX_JSON_COLS = 24
# Rows listed by the generic table view. Every encrypted `data` blob costs a
# SubtleCrypto round-trip plus a JSON column expansion, so tables carrying one
# list fewer rows to stay inside the Worker CPU limit.
ADMIN_TABLE_ROW_LIMIT = 500
ADMIN_ENCRYPTED_ROW_LIMIT = 100


_ADMIN_WALLET_KEY_NAMES = frozenset({
    "donationsecret",
    "walletsecret",
    "walletprivatekey",
    "solanasecret",
    "solanaprivatekey",
    "seedbase64url",
    "mnemonic",
    "recoveryphrase",
})


def _admin_contains_wallet_key(value):
    if isinstance(value, dict):
        normalized_keys = {
            re.sub(r"[^a-z0-9]", "", str(key).lower())
            for key in value
        }
        for key, item in value.items():
            normalized = re.sub(r"[^a-z0-9]", "", str(key).lower())
            if normalized in _ADMIN_WALLET_KEY_NAMES:
                return True
            # Historical bounty rows used the generic pair {address, secret}.
            if normalized == "secret" and "address" in normalized_keys:
                return True
            if _admin_contains_wallet_key(item):
                return True
    elif isinstance(value, list):
        return any(_admin_contains_wallet_key(item) for item in value)
    return False


def _admin_redact_wallet_keys(value):
    if isinstance(value, dict):
        normalized_keys = {
            re.sub(r"[^a-z0-9]", "", str(key).lower())
            for key in value
        }
        redacted = {}
        for key, item in value.items():
            normalized = re.sub(r"[^a-z0-9]", "", str(key).lower())
            if (
                normalized in _ADMIN_WALLET_KEY_NAMES
                or (normalized == "secret" and "address" in normalized_keys)
            ):
                redacted[key] = "<restricted: offline custody migration>"
            else:
                redacted[key] = _admin_redact_wallet_keys(item)
        return redacted
    if isinstance(value, list):
        return [_admin_redact_wallet_keys(item) for item in value]
    return value


def _admin_compact_cell(value, extra_class=""):
    if value is None or value == "":
        return "<td></td>"
    if isinstance(value, bool):
        text = "true" if value else "false"
    elif isinstance(value, (dict, list)):
        text = json.dumps(value, sort_keys=True)
    else:
        text = str(value)
    if len(text) > ADMIN_CELL_TOOLTIP_MAX:
        text = text[:ADMIN_CELL_TOOLTIP_MAX] + "…"
    shown = text
    if len(shown) > ADMIN_CELL_PREVIEW:
        shown = shown[:ADMIN_CELL_PREVIEW] + "…"
    title = (' title="%s"' % _html_escape(text)) if shown != text else ""
    cls = (' class="%s"' % extra_class) if extra_class else ""
    return "<td%s%s>%s</td>" % (cls, title, _html_escape(shown))


def _admin_json_cell(decoded):
    # Collapse the decrypted `data` JSON to a small marker; the full pretty
    # JSON shows in the hover tooltip.
    decoded = _admin_redact_wallet_keys(decoded)
    pretty = json.dumps(decoded, indent=2, sort_keys=True)
    if len(pretty) > ADMIN_CELL_TOOLTIP_MAX:
        pretty = pretty[:ADMIN_CELL_TOOLTIP_MAX] + "…"
    return ('<td class="jsoncell" title="%s">{…} %d key(s)</td>'
            % (_html_escape(pretty), len(decoded)))


# --- Admin bulk select + delete helpers (operate on rowid) -------------------

def _admin_bulk_form_open(table, csrf_field="", admin_query=""):
    # Per-row buttons ride this form with their own formaction (forms cannot
    # nest) and run their own confirmation, so the bulk prompt must not also
    # fire for them — it asked "delete the selected rows?" for a row action
    # that deletes nothing.
    return (
        '<form method="post" action="%s" '
        'onsubmit="return (event.submitter&&'
        "event.submitter.hasAttribute('formaction'))||"
        'confirm(\'Delete the selected '
        'row(s)? This cannot be undone.\')">'
        '%s'
        '<div class="bulk-bar">'
        '<button class="btn btn-sm btn-danger" type="submit">%sDelete selected'
        '</button>'
        '<span class="hint">Tick rows (or the header box for all) then delete.'
        '</span></div>'
    ) % (_admin_href(admin_query, table=table, action="delete_rows"),
         csrf_field, _admin_shell.icon("trash"))


def _admin_select_all_th():
    return (
        '<th style="width:36px"><input type="checkbox" title="Select all" '
        'aria-label="Select all rows" '
        "onclick=\"for(const c of this.closest('table')"
        ".querySelectorAll('input[name=ids]'))c.checked=this.checked\"></th>"
    )


def _admin_row_checkbox(rowid):
    return ('<td><input type="checkbox" name="ids" value="%s" '
            'aria-label="Select row %s"></td>'
            % (_html_escape(rowid), _html_escape(rowid)))


def _admin_error_source(method, path):
    """Stable display classification for the shared operational error log."""
    # Reports posted by the Qt desktop app (POST /api/desktop-errors) share the
    # log with browser and Worker errors: a third source, not a Worker fault.
    if (
        str(method or "").strip().upper() == "APP"
        or str(path or "").startswith("/desktop-error/")
    ):
        return "Desktop"
    is_javascript = (
        str(method or "").strip().upper() == "JS"
        or str(path or "").startswith("/client-error/")
    )
    return "JavaScript" if is_javascript else "Worker"


def _admin_error_source_badge(method, path):
    source = _admin_error_source(method, path)
    return (
        '<span class="src %s">%s</span>'
        % (source.lower(), source)
    )


def _admin_error_related_users(actors, anonymous=0):
    """Summarize which accounts an error (or error group) actually hit.

    `actors` maps account name -> occurrence count. Anonymous occurrences are
    counted separately rather than dropped: "3 signed-in users plus anonymous
    traffic" and "only anonymous traffic" are very different bugs.
    """
    ranked = sorted(
        (actors or {}).items(), key=lambda item: (-item[1], item[0]))
    parts = ["%s (%d)" % (name, hits) for name, hits in ranked]
    if int(anonymous or 0):
        parts.append("anonymous (%d)" % int(anonymous))
    if not parts:
        return "—"
    return ", ".join(parts)


def _admin_error_users_cell(actors, anonymous=0):
    summary = _admin_error_related_users(actors, anonymous)
    shown = summary if len(summary) <= 60 else summary[:60] + "…"
    return (
        '<td class="error-users" title="%s">%s</td>'
        % (_html_escape(summary), _html_escape(shown))
    )


def _admin_error_row_user_cell(actor):
    name = str(actor or "").strip().lower()
    return (
        '<td class="error-users">%s</td>'
        % (_html_escape(name) if name
           else '<span class="error-anonymous">anonymous</span>')
    )


def _admin_error_copy_button(text):
    """Copy the full (untruncated) error text the cell only previews."""
    return (
        '<button type="button" class="btn btn-sm" data-copy="%s" '
        'title="Copy this message" '
        'onclick="event.stopPropagation()">Copy</button>'
        % _html_escape(text or "")
    )


def _admin_error_bot_task_fields(status, method, path, message, users=""):
    return "".join(
        '<input type="hidden" name="%s" value="%s">'
        % (_html_escape(name), _html_escape(value))
        for name, value in (
            ("group_status", status),
            ("group_method", method),
            ("group_path", path),
            ("group_message", message),
            ("group_users", users),
        )
    )


def _admin_error_bot_task_form(
        status, method, path, message, users="",
        csrf_field="", admin_query=""):
    """Send one error (or equivalent-error group) to the organization tasks."""
    return (
        '<form class="inline" method="post" action="%s" '
        '>%s%s<button class="btn btn-sm" type="submit">'
        "Send to task</button></form>"
        % (
            _admin_href(
                admin_query, table="error_log", action="create_bot_task"),
            csrf_field,
            _admin_error_bot_task_fields(
                status, method, path, message, users),
        )
    )


def _admin_error_row_delete_button(rowid, admin_query=""):
    return (
        '<button class="btn btn-sm btn-danger" type="submit" name="error_id" '
        'value="%s" formaction="%s" '
        'onclick="event.stopPropagation();return confirm('
        "'Delete this error? This cannot be undone.')\">Delete</button>"
        % (
            _html_escape(rowid),
            _admin_href(
                admin_query, table="error_log", action="delete_error_row"),
        )
    )


def _admin_error_row_bot_task_button(rowid, admin_query=""):
    # The raw table's rows already live inside the bulk-delete form, so this
    # rides that form with its own formaction (forms cannot nest). Only the
    # row id travels; the server re-reads the stored error itself.
    return (
        '<button class="btn btn-sm" type="submit" name="error_id" '
        'value="%s" formaction="%s" '
        'onclick="event.stopPropagation()">Send to task</button>'
        % (
            _html_escape(rowid),
            _admin_href(
                admin_query, table="error_log", action="create_bot_task"),
        )
    )


def _admin_error_group_delete_form(
        status, method, path, message, csrf_field="", admin_query=""):
    fields = "".join(
        '<input type="hidden" name="%s" value="%s">'
        % (_html_escape(name), _html_escape(value))
        for name, value in (
            ("group_status", status),
            ("group_method", method),
            ("group_path", path),
            ("group_message", message),
        )
    )
    return (
        '<form class="inline" method="post" action="%s" '
        'onsubmit="return confirm('
        "'Delete every error in this group? This cannot be undone.')\">"
        "%s%s<button class=\"btn btn-sm btn-danger\" type=\"submit\">"
        "Delete group</button></form>"
        % (
            _admin_href(
                admin_query, table="error_log",
                action="delete_error_group"),
            csrf_field,
            fields,
        )
    )


def _admin_record_row_attrs(admin_query, table, rowid):
    # _admin_href already emits attribute-ready `&amp;` separators; escaping
    # again left literal "&amp;" in dataset.href, so a row click dropped every
    # parameter after the first and landed back on the default table.
    href = _admin_href(
        admin_query, table=table, action="detail", rowid=rowid)
    return (
        ' class="record-row" tabindex="0" role="link" data-href="%s" '
        'onclick="if(!event.target.closest(\'input,button,a,label\'))'
        'location.href=this.dataset.href" '
        'onkeydown="if(event.key===\'Enter\')location.href=this.dataset.href"'
        % href
    )


async def _admin_table_columns(env, table):
    # Real column names for the table (table name is validated by the caller).
    rows = await d1_all(env, "PRAGMA table_info(" + table + ")")
    return [str(r.get("name", "")) for r in rows if r.get("name")]


async def _render_row_form(env, table, rowid, csrf_field="", admin_query=""):
    # Retained as a fail-closed target for old admin edit/new bookmarks.
    # Generic create/update was an ambient authorization bypass: a platform
    # admin could edit users, org membership, SSH keys, mirror proofs or reward
    # state without satisfying the domain API's signature and policy checks.
    # Keep this helper as a fail-closed response for stale bookmarks.
    return (
        '<div class="empty">Generic row editing is disabled. Use the '
        "purpose-built, audited administration action for this resource.</div>"
    )


async def _render_record_detail(env, table, rowid, admin_query=""):
    back = '<a href="%s">%s</a>%s' % (
        _admin_href(admin_query, view="database", table=table),
        _html_escape(table), _admin_shell.icon("chev"))
    if table in ADMIN_HIDDEN_TABLES:
        return _admin_shell.card(_admin_shell.empty(
            "This table is restricted."))
    try:
        rowid = int(rowid)
    except (TypeError, ValueError):
        return _admin_shell.card(_admin_shell.empty("Record not found."))
    if rowid <= 0:
        return _admin_shell.card(_admin_shell.empty("Record not found."))
    row = await d1_first(
        env,
        "SELECT rowid AS _rowid_, * FROM " + table + " WHERE rowid=? LIMIT 1",
        rowid,
    )
    if not row:
        return _admin_shell.card(_admin_shell.empty("Record not found."))
    fields = [("_rowid_", rowid)]
    for key, value in row.items():
        if key == "_rowid_":
            continue
        if key == "data" and isinstance(value, str) and value:
            decoded = await decrypt_row(env, value)
            value = (
                _admin_redact_wallet_keys(decoded)
                if isinstance(decoded, (dict, list))
                else decoded
            )
        if isinstance(value, (dict, list)):
            value = json.dumps(value, indent=2, sort_keys=True, default=str)
        elif value is None:
            value = "NULL"
        fields.append((key, value))
    rows = "".join(
        '<dt class="mono">%s</dt><dd class="mono">%s</dd>'
        % (_html_escape(key), _html_escape(value))
        for key, value in fields
    )
    return (
        '<section class="card record-detail"><div class="card-h"><div>'
        '<div class="crumb">%s row %d</div><h2>Record detail</h2>'
        '<p>Read-only vertical view. Sensitive tables remain restricted and '
        'encrypted data is redacted before display.</p></div>'
        '<a class="btn btn-sm" href="%s">Back to %s</a></div>'
        '<div class="card-b"><dl class="kv">%s</dl></div></section>'
        % (
            back,
            rowid,
            _admin_href(admin_query, view="database", table=table),
            _html_escape(table),
            rows,
        )
    )


async def _admin_row_values(env, table, form):
    # Map submitted f_<col> fields to (column, value) for valid columns only,
    # encrypting the `data` column from its edited JSON.
    valid = set(await _admin_table_columns(env, table))
    cols, values = [], []
    for key, vals in form.items():
        if not key.startswith("f_"):
            continue
        col = key[2:]
        if col not in valid:
            continue
        raw = vals[0] if vals else ""
        if col == "data":
            parsed = json.loads(raw) if raw.strip() else {}
            if _admin_contains_wallet_key(parsed):
                raise ValueError("wallet_private_key_fields_are_not_accepted")
            values.append(await encrypt_row(env, parsed))
        else:
            values.append(raw)
        cols.append(col)
    return cols, values


async def _admin_update_row(env, table, form):
    return (
        "Update blocked: generic database mutation is disabled; use the "
        "purpose-built audited administration action."
    )


async def _admin_insert_row(env, table, form):
    return (
        "Insert blocked: generic database mutation is disabled; use the "
        "purpose-built audited administration action."
    )


async def _admin_selected_rows_have_wallet_keys(env, table, rowids):
    # A bulk delete must not become an undeclared custody scrub. Only tables
    # that can historically embed donation_secret are inspected; key-bearing
    # rows remain until the separately confirmed offline migration updates them.
    if table not in ("users", "nodes") or not rowids:
        return False
    for start in range(0, len(rowids), 90):
        batch = rowids[start:start + 90]
        placeholders = ",".join(["?"] * len(batch))
        rows = await d1_all(
            env,
            "SELECT data FROM " + table + " WHERE rowid IN ("
            + placeholders + ")",
            *batch,
        )
        for row in rows or []:
            decoded = await decrypt_row(env, row.get("data", ""))
            if _admin_contains_wallet_key(decoded):
                return True
    return False


async def _admin_selected_rows_digest(env, table, rowids):
    """Return a content-free audit digest for a bounded operational purge."""
    if not _admin_purge_allowed(table) or not rowids:
        return ""
    records = []
    for start in range(0, min(len(rowids), 500), 90):
        batch = rowids[start:start + 90]
        placeholders = ",".join(["?"] * len(batch))
        rows = await d1_all(
            env,
            "SELECT rowid AS _rowid_,* FROM " + table
            + " WHERE rowid IN (" + placeholders + ") ORDER BY rowid",
            *batch,
        )
        records.extend(rows or [])
    canonical = json.dumps(
        records,
        sort_keys=True,
        separators=(",", ":"),
        default=str,
    ).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _render_diag_breakdown(title, pairs):
    if not pairs:
        return ""
    total = sum(int(n or 0) for _k, n in pairs) or 1
    meters = "".join(
        _admin_shell.meter(k or "(unknown)", 100.0 * int(n or 0) / total,
                           "%s run%s" % (n, "" if int(n or 0) == 1 else "s"))
        for k, n in pairs)
    return _admin_shell.card(meters, title=title)


def _render_install_diag_overview(summary):
    # Anonymous install funnel: per-step distinct-run counts (reached / ok /
    # failed) plus coarse platform breakdowns. Counts are over a 30-day window.
    shell = _admin_shell
    total = int(summary.get("total", 0) or 0)
    completed = int(summary.get("completed", 0) or 0)
    funnel = summary.get("funnel", [])
    failed_runs = sum(int(f.get("failed") or 0) for f in funnel)
    worst = max(funnel, key=lambda f: int(f.get("failed") or 0), default=None)
    platforms = summary.get("platforms", [])
    top_platform = platforms[0] if platforms else None
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("Install runs", shell.format_int(total), "Last 30 days")
        + shell.kpi("Completed", shell.format_int(completed),
                    ("%d%% reached the final step" % round(100 * completed / total))
                    if total else "No runs recorded")
        + shell.kpi("Failed steps", shell.format_int(failed_runs),
                    ("Most at %s" % worst["step"])
                    if worst and int(worst.get("failed") or 0) else
                    "No step failed", icon_name="alert",
                    tone="bad" if failed_runs else "")
        + shell.kpi("Most common platform",
                    top_platform[0] if top_platform else "—",
                    ("%d run%s" % (top_platform[1],
                                   "" if top_platform[1] == 1 else "s"))
                    if top_platform else "No runs recorded")
        + '</div>')
    funnel_rows = []
    for f in funnel:
        runs = int(f["runs"] or 0)
        ok = int(f["ok"] or 0)
        failed = int(f["failed"] or 0)
        pct = round(100 * ok / runs) if runs else 0
        funnel_rows.append(
            "<tr><td class=\"mono\">%s</td><td class=\"r\">%s</td>"
            "<td class=\"r\">%s</td><td class=\"r%s\">%s</td>"
            "<td>%s</td><td class=\"r\">%s</td></tr>"
            % (_html_escape(f["step"]), _html_escape(runs), _html_escape(ok),
               " s5" if failed else "", _html_escape(failed),
               shell.hbar(pct, "bad" if failed else ""),
               ("%d%%" % pct) if runs else "—"))
    funnel_card = shell.card(
        title="Install funnel", description=(
            "Distinct anonymous runs over 30 days. Each run is a random id, "
            "never tied to an account, email or IP."),
        table=shell.table(
            "<th>Step</th><th class=\"r\">Reached</th><th class=\"r\">OK</th>"
            "<th class=\"r\">Failed</th><th>Success</th><th class=\"r\">OK %</th>",
            funnel_rows, "No install events recorded yet."))
    breakdowns = "".join(
        _render_diag_breakdown(title, summary.get(key, []))
        for title, key in (("Platform", "platforms"),
                           ("Package manager", "managers"),
                           ("Distribution", "distros")))
    return (
        kpis
        + '<div class="grid split">' + funnel_card
        + '<div class="stack">' + (breakdowns or shell.card(
            shell.empty("No platform data yet."), title="Platforms"))
        + '</div></div>'
    )


# Nav row counts COUNT(*) every application table, i.e. D1 bills a read for
# every row in the database on each admin page view. They only orient the
# operator, so d1_read_guard's colo-wide copy serves every view for its TTL;
# any admin POST (the only way this console changes rows) forgets it.
async def _admin_table_counts_cached(env, tables, refresh=False):
    # The console nav and the desktop Database tab ask for different table
    # lists, so the one shared copy always counts every table either could
    # name; otherwise each caller would read the other's copy as zeros.
    async def count_all():
        names = set(tables) | set(await _admin_list_tables(env))
        return await _admin_table_counts(env, sorted(names))

    if refresh:
        await _admin_table_counts_forget()
    counts = await d1_shared_read(env, "admin-table-counts", count_all)
    if any(t not in (counts or {}) for t in tables):
        # A migration added a table since the copy was taken.
        await _admin_table_counts_forget()
        counts = await d1_shared_read(env, "admin-table-counts", count_all)
    return {t: int((counts or {}).get(t, 0) or 0) for t in tables}


async def _admin_table_counts_forget():
    await d1_shared_forget("admin-table-counts")


# Users directory. Only plaintext columns sort server-side: email, status and
# the rest live in the encrypted `data` blob, and sorting on them would mean
# decrypting every account on every page view.
ADMIN_USER_SORTS = {
    "joined": "rowid",
    "username": "username",
    "admin": "is_admin",
    "ios": "early_ios_tester",
    "origin": "signup_origin",
}
ADMIN_USER_PAGE_SIZES = (25, 50, 100)
ADMIN_USER_PAGE_SIZE = 50


def _admin_users_state(params):
    params = params or {}

    def first(key):
        values = params.get(key) or [""]
        return str(values[0] or "")

    order = first("order")
    if order not in ADMIN_USER_SORTS:
        order = "joined"
    direction = first("dir")
    if direction not in ("asc", "desc"):
        direction = "desc" if order == "joined" else "asc"
    try:
        per = int(first("per"))
    except ValueError:
        per = ADMIN_USER_PAGE_SIZE
    if per not in ADMIN_USER_PAGE_SIZES:
        per = ADMIN_USER_PAGE_SIZE
    try:
        page = max(1, int(first("page")))
    except ValueError:
        page = 1
    query = clean_string(first("q"), MAX_NODE_NAME).strip().lower()
    nav_sort = first("sort") if first("sort") in ("name", "records") else ""
    return {"order": order, "dir": direction, "per": per, "page": page,
            "q": query, "sort": nav_sort}


def _admin_users_href(admin_query, state, **overrides):
    merged = dict(state)
    merged.update(overrides)
    return _admin_href(
        admin_query, view="users", q=merged["q"],
        order=merged["order"], dir=merged["dir"],
        per=merged["per"] if merged["per"] != ADMIN_USER_PAGE_SIZE else "",
        page=merged["page"] if merged["page"] > 1 else "")


def _admin_user_href(admin_query, name):
    return _admin_href(admin_query, view="users", user=name)


def _admin_users_sort_th(admin_query, state, key, label, align=""):
    active = state["order"] == key
    if active:
        direction = "asc" if state["dir"] == "desc" else "desc"
        arrow = " ▲" if state["dir"] == "asc" else " ▼"
        aria = ' aria-sort="%s"' % (
            "ascending" if state["dir"] == "asc" else "descending")
    else:
        direction = "desc" if key == "joined" else "asc"
        arrow = ""
        aria = ""
    return '<th%s%s><a class="sortlink%s" href="%s">%s%s</a></th>' % (
        (' class="%s"' % align) if align else "",
        aria,
        " on" if active else "",
        _admin_users_href(admin_query, state, order=key, dir=direction,
                          page=1),
        _html_escape(label), arrow)


def _admin_users_pager(admin_query, state, total, pages):
    page = state["page"]
    start = (page - 1) * state["per"] + 1 if total else 0
    end = min(total, page * state["per"])

    def link(label, target, enabled):
        if not enabled:
            return ('<span class="btn btn-sm pagelink" aria-disabled="true" '
                    'style="opacity:.5">%s</span>' % label)
        return '<a class="btn btn-sm pagelink" href="%s">%s</a>' % (
            _admin_users_href(admin_query, state, page=target), label)

    return (
        '<div class="pager">'
        '<span>Showing %d–%d of %d</span>'
        '<div class="actions">%s%s<span class="pagecur">Page %d of %d</span>'
        '%s%s</div></div>'
        % (start, end, total,
           link("« First", 1, page > 1),
           link("‹ Prev", page - 1, page > 1),
           page, pages,
           link("Next ›", page + 1, page < pages),
           link("Last »", pages, page < pages))
    )


async def _render_admin_users_list(env, params, csrf_field="",
                                   admin_query="", prefix=""):
    shell = _admin_shell
    state = _admin_users_state(params)
    where, args = "", []
    if state["q"]:
        # Usernames are stored lowercased, so a half-open range is a
        # case-insensitive prefix match that can still use idx_users_username.
        where = " WHERE username >= ? AND username < ?"
        args = [state["q"], state["q"] + "￿"]
    count_row = await d1_first(
        env, "SELECT COUNT(*) AS n FROM users" + where, *args)
    total = int((count_row or {}).get("n", 0) or 0)
    pages = max(1, -(-total // state["per"]))
    state["page"] = min(state["page"], pages)
    direction = "ASC" if state["dir"] == "asc" else "DESC"
    rows = await d1_all(
        env,
        "SELECT rowid AS _rowid_, data, username, is_admin, "
        "early_ios_tester, signup_origin "
        "FROM users" + where + " ORDER BY "
        + ADMIN_USER_SORTS[state["order"]] + " " + direction
        + ", rowid " + direction + " LIMIT ? OFFSET ?",
        *args, state["per"], (state["page"] - 1) * state["per"],
    )
    now = _admin_now()

    hidden = "".join(
        '<input type="hidden" name="%s" value="%s">'
        % (key, _html_escape(value))
        for key, value in (
            ("admin", admin_query.split("=", 1)[1] if admin_query else ""),
            ("view", "users"),
            ("order", state["order"]), ("dir", state["dir"]))
        if value
    )
    per_options = "".join(
        '<option value="%d"%s>%d per page</option>'
        % (size, " selected" if size == state["per"] else "", size)
        for size in ADMIN_USER_PAGE_SIZES
    )
    search = (
        '<form class="toolbar usersearch" method="get" action="">%s'
        '<label class="sr" for="users-q">Username starts with</label>'
        '<input id="users-q" type="search" name="q" value="%s" '
        'placeholder="Username starts with…" autocomplete="off" '
        'style="width:280px;max-width:100%%">'
        '<label class="sr" for="users-per">Rows per page</label>'
        '<select id="users-per" name="per" onchange="this.form.submit()">%s'
        '</select><button class="btn" type="submit">Search</button>%s'
        '<div class="grow"></div><span class="hint">%s</span></form>'
        % (hidden, _html_escape(state["q"]), per_options,
           ('<a class="btn" href="%s">Clear</a>'
            % _admin_users_href(admin_query, state, q="", page=1))
           if state["q"] else "",
           "%d account(s)%s" % (
               total,
               (' matching “%s”' % _html_escape(state["q"]))
               if state["q"] else ""))
    )
    pager = _admin_users_pager(admin_query, state, total, pages)
    if not rows:
        return (prefix + '<section class="card">' + search
                + shell.empty("No users match.") + '</section>')

    body = []
    for r in rows:
        rec = await decrypt_row(env, r.get("data")) if r.get("data") else None
        rec = rec if isinstance(rec, dict) else {}
        name = str(r.get("username") or rec.get("name") or "")
        href = _admin_user_href(admin_query, name)
        verified = rec.get("email_verified") is True
        status = str(rec.get("status") or "")
        origin = str(r.get("signup_origin") or "unknown")
        body.append(
            '<tr class="record-row" tabindex="0" role="link" '
            'data-href="%s" '
            'onclick="if(!event.target.closest(\'input,button,a,label\'))'
            'location.href=this.dataset.href" '
            'onkeydown="if(event.key===\'Enter\')'
            'location.href=this.dataset.href">'
            % href
            + _admin_row_checkbox(r.get("_rowid_", ""))
            + '<td><a class="who" href="%s"><span class="avatar" '
              'aria-hidden="true">%s</span><span><b>%s</b><span>%s</span>'
              '</span></a></td>' % (
                  href, _html_escape(shell.initials(name)),
                  _html_escape(name),
                  _html_escape(rec.get("email") or "no email"))
            + "<td>%s</td>" % (
                shell.pill("verified", "ok", "check") if verified else
                shell.pill("unverified", "warn") if rec.get("email") else
                '<span class="muted">—</span>')
            + "<td>%s</td>" % (
                shell.pill(status, "bad")
                if status and status != "active" else
                '<span class="muted">%s</span>' % _html_escape(status or "—"))
            + "<td>%s</td>" % (
                shell.pill("admin", "done")
                if int(r.get("is_admin") or 0) else "")
            + "<td>%s</td>" % (
                shell.pill("early iOS", "info")
                if int(r.get("early_ios_tester") or 0) else "")
            + "<td>%s</td>" % shell.pill(
                origin, {"organic": "info", "referral": "ok",
                         "job_flow": "done"}.get(origin, ""))
            + '<td class="r">%s</td>' % shell.time_html(
                rec.get("created_at"), now)
            + "</tr>"
        )
    head = (
        _admin_select_all_th()
        + _admin_users_sort_th(admin_query, state, "username", "User")
        + "<th>Verified</th><th>Status</th>"
        + _admin_users_sort_th(admin_query, state, "admin", "Admin")
        + _admin_users_sort_th(admin_query, state, "ios", "iOS tester")
        + _admin_users_sort_th(admin_query, state, "origin", "Signup origin")
        + _admin_users_sort_th(admin_query, state, "joined", "Joined", "r")
    )
    return (
        prefix + '<section class="card">' + search
        + '<div class="card-t">'
        + _admin_bulk_form_open("users", csrf_field, admin_query)
        + '<table class="users"><thead><tr>' + head
        + "</tr></thead><tbody>" + "".join(body) + "</tbody></table></form>"
        + "</div>" + pager + "</section>"
    )


def _admin_checkbox(name, label, checked):
    return _admin_shell.switch(name, label, bool(checked))


def _render_admin_user_editor(row, name, csrf_field="", admin_query=""):
    shell = _admin_shell
    rec = row["rec"]
    now = _admin_now()
    facts = (
        ("Username", _html_escape(name)),
        ("Kind", _html_escape(_account_kind(rec))),
        ("Status", _html_escape(rec.get("status") or "—")),
        ("Administrator", "yes" if int(row.get("is_admin") or 0) else "no"),
        ("Early iOS tester",
         "yes" if int(row.get("early_ios_tester") or 0) else "no"),
        ("Signup origin", _html_escape(row.get("signup_origin") or "unknown")),
        ("Email verification", _html_escape(
            "%s%s" % (
                rec.get("email_verification_method") or "verified",
                (" by " + str(rec.get("email_verified_by")))
                if rec.get("email_verified_by") else "")
            if rec.get("email_verified") is True else "not verified")),
    )
    fact_rows = "".join(
        "<dt>%s</dt><dd>%s</dd>" % (_html_escape(k), v) for k, v in facts)
    for label, key in (("Joined", "created_at"),
                       ("Last email", "last_email_ts")):
        if rec.get(key):
            fact_rows += "<dt>%s</dt><dd>%s</dd>" % (
                label, shell.time_html(rec.get(key), now))

    def text_field(field, label, value, maxlength, kind="text"):
        return (
            '<div class="field"><label for="user-%s">%s</label>'
            '<input id="user-%s" type="%s" name="%s" value="%s" '
            'maxlength="%d" autocomplete="off"></div>'
            % (field, label, field, kind, field, _html_escape(value),
               maxlength))

    return (
        '<div class="user-grid">'
        '<section class="card user-editor">'
        '<div class="card-h"><div><h2>Edit account</h2>'
        '<p>Every save is recorded in the sensitive audit log. Payout '
        'addresses, keys and the username are deliberately not editable '
        'here.</p></div></div>'
        '<form class="card-b" method="post" action="%s" '
        'onsubmit="return confirm(\'Save these changes to %s?\')">%s'
        '<input type="hidden" name="name" value="%s">'
        '<div class="form-grid">'
        '%s'
        '%s'
        '<div class="field wide"><label for="user-profile_bio">Bio</label>'
        '<textarea id="user-profile_bio" name="profile_bio" rows="3" '
        'maxlength="%d">%s</textarea></div>'
        '%s%s'
        '<div class="wide form-row" style="gap:24px">%s%s%s%s%s%s</div>'
        '<div class="wide form-row" style="justify-content:flex-end">'
        '<a class="btn" href="%s">Cancel</a>'
        '<button class="btn btn-primary" type="submit">Save changes</button>'
        '</div></div>'
        '</form></section>'
        '<section class="card record-detail"><div class="card-h"><h2>Summary'
        '</h2></div><div class="card-b"><dl class="kv">%s</dl></div></section>'
        '</div>'
        % (
            _admin_href(admin_query, view="users", user=name,
                        action="update_user"),
            _html_escape(name).replace("'", "&#39;"),
            csrf_field,
            _html_escape(name),
            text_field("email", "Email", rec.get("email", ""), 254, "email"),
            text_field("profile_location", "Location",
                       rec.get("profile_location", ""), MAX_PROFILE_LOCATION),
            MAX_PROFILE_BIO,
            _html_escape(rec.get("profile_bio", "")),
            text_field("profile_timezone", "Timezone (IANA, e.g. Europe/Paris)",
                       rec.get("profile_timezone", ""), MAX_PROFILE_TIMEZONE),
            '<div class="field"><span class="label">Verification</span>%s</div>'
            % _admin_checkbox(
                "email_verified", "Email verified",
                rec.get("email_verified") is True),
            _admin_checkbox("profile_private", "Private profile",
                            bool(rec.get("profile_private"))),
            _admin_checkbox("followers_public", "Followers list public",
                            bool(rec.get("profile_followers_public"))),
            _admin_checkbox("email_notifications", "Email notifications",
                            bool(rec.get("email_notifications", True))),
            # Publishing an address is the member's choice; the console can
            # only take a published one down.
            (_admin_checkbox("clear_public_email", "Hide public email", False)
             if rec.get("profile_email_public") else "")
            + (_admin_checkbox("reset_totp",
                               "Reset two-factor authentication", False)
               if rec.get("totp_enrolled") or rec.get("totp_pending_secret")
               else ""),
            _admin_checkbox("is_admin", "Administrator",
                            bool(int(row.get("is_admin") or 0))),
            _admin_checkbox("early_ios_tester", "Early iOS tester",
                            bool(int(row.get("early_ios_tester") or 0))),
            _admin_href(admin_query, view="users"),
            fact_rows,
        )
    )


async def _admin_update_user(env, request, form, actor):
    """Apply the admin console's account edit form.

    A purpose-built action, not generic row editing: only profile/contact
    fields and the users.is_admin and users.early_ios_tester flags are
    accepted, each validated exactly
    like the self-service account update, and the audit record names the
    fields that changed (never their values). An administrator cannot revoke
    their own rights here. Returns (banner, audit_details).
    """
    def field(key):
        return str((form.get(key) or [""])[0] or "")

    def flag(key):
        return field(key) == "1"

    name = clean_string(field("name"), MAX_NODE_NAME).strip().lower()
    if not name or not valid_node_name(name):
        return "Update user failed: invalid account name.", {}
    name_bi, rec = await _account_row(env, name)
    if not rec or _account_kind(rec) != "user":
        return ("Update user failed: no user account named '%s'." % name,
                {"reason": "no_such_user"})
    before = dict(rec)
    changed = []
    admin_row = await d1_first(
        env, "SELECT is_admin, early_ios_tester FROM users "
        "WHERE user_bi=? LIMIT 1", name_bi)
    was_admin = bool(int((admin_row or {}).get("is_admin") or 0))
    make_admin = flag("is_admin")
    was_ios_tester = bool(
        int((admin_row or {}).get("early_ios_tester") or 0))
    make_ios_tester = flag("early_ios_tester")
    if (was_admin and not make_admin
            and str(actor or "").strip().lower() == name):
        # Keep the console reachable: another administrator has to do it.
        return ("Update user failed: you cannot remove your own "
                "administrator rights.", {"reason": "self_admin_revoke"})
    if flag("reset_totp") and str(actor or "").strip().lower() == name:
        return ("Update user failed: you cannot reset your own two-factor "
                "authentication.", {"reason": "self_totp_reset"})

    raw_email = field("email").strip()
    email = clean_string(rec.get("email", ""), 254).strip().lower()
    new_email_bi = None
    if raw_email:
        wanted = _waitlist_normalize_email(raw_email)
        if not wanted:
            return ("Update user failed: '%s' is not a valid email address."
                    % raw_email[:254], {"reason": "bad_email"})
        if wanted != email:
            new_email_bi = await blind_index(env, wanted)
            taken = await d1_first(
                env,
                "SELECT 1 AS found FROM users WHERE email_bi=? "
                "AND user_bi<>? LIMIT 1",
                new_email_bi, name_bi)
            if taken:
                return ("Update user failed: that email address belongs to "
                        "another account.", {"reason": "email_taken"})
            rec["email"] = wanted
            # The public-email flag means "publish the address this person
            # confirmed"; an address an administrator typed is not that.
            rec.pop("profile_email_public", None)
            email = wanted
            changed.append("email")
    elif email:
        return ("Update user failed: the email address cannot be cleared.",
                {"reason": "email_required"})

    timezone_raw = clean_string(
        field("profile_timezone"), MAX_PROFILE_TIMEZONE).strip()
    timezone = _clean_profile_timezone(timezone_raw)
    if timezone_raw and not timezone:
        return ("Update user failed: '%s' is not a known timezone."
                % timezone_raw, {"reason": "bad_profile_timezone"})

    for key, value in (
            ("profile_bio",
             clean_string(field("profile_bio"), MAX_PROFILE_BIO).strip()),
            ("profile_location",
             clean_string(field("profile_location"),
                          MAX_PROFILE_LOCATION).strip()),
            ("profile_timezone", timezone)):
        if rec.get(key, "") != value:
            if value:
                rec[key] = value
            else:
                rec.pop(key, None)
            changed.append(key)

    for form_key, rec_key in (("profile_private", "profile_private"),
                              ("followers_public",
                               "profile_followers_public")):
        if bool(rec.get(rec_key)) != flag(form_key):
            if flag(form_key):
                rec[rec_key] = True
            else:
                rec.pop(rec_key, None)
            changed.append(rec_key)

    if bool(rec.get("email_notifications", True)) != flag(
            "email_notifications"):
        if flag("email_notifications"):
            rec.pop("email_notifications", None)
        else:
            rec["email_notifications"] = False
        changed.append("email_notifications")

    if flag("clear_public_email") and rec.get("profile_email_public"):
        rec.pop("profile_email_public", None)
        changed.append("profile_email_public")

    if flag("reset_totp") and (
            rec.get("totp_enrolled") or rec.get("totp_pending_secret")):
        for key in ("totp_enrolled", "totp_secret", "totp_enrolled_at",
                    "totp_pending_secret"):
            rec.pop(key, None)
        changed.append("totp")

    was_verified = before.get("email_verified") is True
    newly_verified = False
    if flag("email_verified"):
        if not email:
            return ("Update user failed: an account without an email address "
                    "cannot be marked verified.",
                    {"reason": "account_has_no_email"})
        if not was_verified or "email" in changed:
            rec["email_verified"] = True
            rec["email_verified_at"] = int(Date.now())
            # Same stamp as the signed admin override: nobody proved control
            # of the mailbox, an administrator vouched for it.
            rec["email_verification_method"] = "admin_override"
            rec["email_verified_by"] = actor
            newly_verified = not was_verified
            changed.append("email_verified")
    elif was_verified:
        rec["email_verified"] = False
        for key in ("email_verified_at", "email_verification_method",
                    "email_verified_by"):
            rec.pop(key, None)
        changed.append("email_verified")

    admin_changed = was_admin != make_admin
    ios_tester_changed = was_ios_tester != make_ios_tester
    if not changed and not admin_changed and not ios_tester_changed:
        return "No changes to save for '%s'." % name, {"fieldCount": 0}
    try:
        if changed:
            await _save_account(env, name_bi, rec, email_bi=new_email_bi)
    except Exception as error:
        if "UNIQUE constraint failed: users.email_bi" in str(error):
            return ("Update user failed: that email address belongs to "
                    "another account.", {"reason": "email_taken"})
        if "legacy_custody_migration_required" in str(error):
            return ("Update user blocked: this record still holds legacy "
                    "wallet custody material.",
                    {"reason": "legacy_custody_migration_required"})
        raise
    if admin_changed:
        await d1_run(
            env, "UPDATE users SET is_admin=? WHERE user_bi=?",
            1 if make_admin else 0, name_bi)
        changed.append("is_admin")
    if ios_tester_changed:
        await d1_run(
            env, "UPDATE users SET early_ios_tester=? WHERE user_bi=?",
            1 if make_ios_tester else 0, name_bi)
        changed.append("early_ios_tester")
    if rec.get("email_verified") is True:
        await d1_run(
            env, "DELETE FROM pending_verifications WHERE name_bi=?", name_bi)
    elif "email" in changed:
        pending = await d1_first(
            env, "SELECT 1 AS found FROM pending_verifications "
            "WHERE name_bi=?", name_bi)
        if pending:
            await _enqueue_verification(env, name_bi, name, email)
    if newly_verified:
        # Referral credit still runs its own maturity and risk gates.
        await _finalize_referral_signup(env, name_bi)
        try:
            await _hackathon.on_email_verified(env, name_bi, name)
        except Exception:
            pass
    _users_directory_memos_clear()
    for cache_key in (ACCOUNT_LOOKUP_CACHE_PREFIX + quote(name),
                      USERS_DIRECTORY_CACHE_KEY, USERS_WORLD_CACHE_KEY):
        try:
            await edge_cache_delete(cache_key)
        except Exception:
            pass
    federated = ("profile_bio", "profile_private", "profile_followers_public")
    if any(key in changed for key in federated):
        await _best_effort_inbox_side_effect(_ap_broadcast_actor_update(
            env, request, AP_ACTOR_USER, name))
    return (
        "Saved '%s': %s." % (name, ", ".join(changed)),
        {"fields": ",".join(changed), "fieldCount": len(changed),
         "adminVerifiedOverride": newly_verified,
         "adminGranted": admin_changed and make_admin,
         "adminRevoked": admin_changed and not make_admin,
         "earlyIosTesterGranted": ios_tester_changed and make_ios_tester,
         "earlyIosTesterRevoked": ios_tester_changed and not make_ios_tester},
    )


async def _render_table_view(
        env, table, csrf_field="", admin_query="", user_filter="",
        known_total=None, params=None):
    # Generic "show all rows" view for one D1 table. The encrypted `data` column
    # (users/nodes/repos/inboxes store an AES-GCM blob there) is decrypted in place
    # so the admin can actually read it. The table name is validated by the
    # caller against the live table list, so it is safe to interpolate.
    # rowid lets the admin select + bulk-delete any row regardless of the table's
    # declared primary key (all these tables are rowid tables).
    # Newest-first by default: rowid ascends with insertion, so ORDER BY rowid DESC
    # surfaces the most recent rows (and, with LIMIT, keeps the newest ones) for
    # every table — including the error log, which reads from these rows.
    shell = _admin_shell
    if table in ADMIN_HIDDEN_TABLES:
        return shell.card(shell.empty("This table is restricted."))
    requested_user = (
        clean_string(user_filter or "", MAX_NODE_NAME).strip().lower()
        if table == "users" else ""
    )
    if requested_user:
        requested_user_bi = await blind_index(env, requested_user)
        rows = await d1_all(
            env,
            "SELECT rowid AS _rowid_, * FROM users "
            "WHERE user_bi=? LIMIT 1",
            requested_user_bi,
        )
        total = len(rows)
        row_limit = 1
    elif table == "users":
        # The users directory pages itself (_render_admin_users_list), so it
        # never pays for the generic newest-100 read and decrypt.
        rows, total, row_limit = [], 0, 0
    else:
        row_limit = ADMIN_TABLE_ROW_LIMIT
        if (table not in ("install_diag", "error_log")
                and "data" in await _admin_table_columns(env, table)):
            row_limit = ADMIN_ENCRYPTED_ROW_LIMIT
        rows = await d1_all(
            env, "SELECT rowid AS _rowid_, * FROM " + table
            + " ORDER BY rowid DESC LIMIT %d" % row_limit)
        if known_total is None:
            count_row = await d1_first(
                env, "SELECT COUNT(*) AS n FROM " + table)
            total = int((count_row or {}).get("n", 0) or 0)
        else:
            total = int(known_total)
    now = _admin_now()

    # Admin tool: reset any user account's login password. Shown beside the
    # users directory; posts back to ?action=set_password (handled in _admin).
    prefix = ""
    if table == "users":
        user_value = (
            ' value="%s"' % _html_escape(requested_user)
            if requested_user else ""
        )
        detail_heading = (
            '<div class="head" id="user-detail"><div><div class="crumb">'
            '<a href="%s">Back to all users</a></div><h1>%s</h1>'
            '<p>Account detail and audited account tools.</p></div></div>'
            % (
                _admin_href(admin_query, view="users"),
                _html_escape(requested_user),
            )
            if requested_user else ""
        )
        prefix = (
            detail_heading +
            '<div class="grid cols-2">'
            '<section class="card"><div class="card-h"><div><h2>Set password'
            '</h2><p>Resets a user account\'s login password (PBKDF2-hashed); '
            'email and payout address are left unchanged.</p></div></div>'
            '<form class="card-b form-row" method="post" action="%s" '
            'onsubmit="return confirm(\'Set a new login password for this '
            'account?\')">'
            + csrf_field +
            '<input type="text" name="name" placeholder="user name"%s '
            'autocomplete="off" required aria-label="User name" class="grow">'
            '<input type="password" name="password" '
            'placeholder="new password (min 8 chars)" minlength="8" required '
            'aria-label="New password" class="grow">'
            '<button class="btn btn-primary" type="submit">Set password'
            '</button></form></section>'
            '<section class="card"><div class="card-h"><div><h2>Resend '
            'verification</h2><p>Re-sends the email-confirmation link to a '
            'user\'s stored address; queues it for manual verification if '
            'email is not configured.</p></div></div>'
            '<form class="card-b form-row" method="post" action="%s" '
            'onsubmit="return confirm(\'Resend the verification email for this '
            'account?\')">'
            + csrf_field +
            '<input type="text" name="name" placeholder="user name"%s '
            'autocomplete="off" required aria-label="User name" class="grow">'
            '<button class="btn" type="submit">Resend verify email</button>'
            '</form></section>'
            '</div>'
        ) % (
            _admin_href(admin_query, view="users", action="set_password"),
            user_value,
            _admin_href(admin_query, view="users", action="resend_verify"),
            user_value,
        )
        if not requested_user:
            return await _render_admin_users_list(
                env, params, csrf_field, admin_query,
                shell.page_head(
                    "Users", "Every account. Open one to edit it, reset its "
                    "password or resend its verification email.",
                    crumb="People")
                + prefix[len(detail_heading):])
        if not rows:
            return (detail_heading
                    + shell.card(shell.empty(
                        "No user account named “%s”." % requested_user)))
        row = dict(rows[0])
        decoded = await decrypt_row(env, row.get("data")) if row.get(
            "data") else None
        row["rec"] = decoded if isinstance(decoded, dict) else {}
        return (
            detail_heading
            + _render_admin_user_editor(
                row, requested_user, csrf_field, admin_query)
            + '<div style="height:16px"></div>'
            + prefix[len(detail_heading):]
            + '<p class="note"><a href="%s">Raw record (read-only)</a></p>'
            % _admin_href(admin_query, view="database", table="users",
                          action="detail", rowid=row.get("_rowid_", ""))
        )

    if table == "install_diag":
        # Purpose-built anonymous install funnel + recent events, newest first.
        summary = await install_diag_summary(env)
        recent = await d1_all(
            env,
            "SELECT rowid AS _rowid_, * FROM install_diag ORDER BY id DESC LIMIT 200")
        body = []
        for r in recent:
            ok = int(r.get("ok", 0) or 0)
            body.append(
                "<tr%s>"
                % _admin_record_row_attrs(
                    admin_query, table, r.get("_rowid_", ""))
                + _admin_row_checkbox(r.get("_rowid_", ""))
                + '<td>%s</td>'
                '<td class="mono">%s</td><td>%s</td><td>%s</td>'
                '<td class="mono">%s</td>'
                "<td>%s</td><td>%s</td><td class=\"muted\">%s</td></tr>"
                % (shell.time_html(r.get("ts", ""), now),
                   _html_escape(r.get("step", "")),
                   shell.pill("ok", "ok") if ok else shell.pill("fail", "bad"),
                   _html_escape(r.get("os", "")), _html_escape(r.get("arch", "")),
                   _html_escape(r.get("pm", "")),
                   _html_escape(r.get("distro", "") or "—"),
                   _html_escape(r.get("detail", "") or "—"))
            )
        if not body:
            events = shell.empty("No install events recorded yet.")
        else:
            events = (_admin_bulk_form_open(table, csrf_field, admin_query)
                      + "<table><thead><tr>" + _admin_select_all_th()
                      + "<th>Time</th><th>Step</th><th>Result</th><th>OS</th>"
                      "<th>Arch</th><th>PM</th><th>Distro</th><th>Detail</th>"
                      "</tr></thead><tbody>" + "".join(body)
                      + "</tbody></table></form>")
        return (
            shell.page_head(
                "Install diagnostics",
                "The anonymous install-script funnel: which step failed, on "
                "which OS, architecture and package manager.",
                crumb="Reliability")
            + _render_install_diag_overview(summary)
            + '<section class="card"><div class="card-h"><div><h2>Recent events'
            '</h2><p>%d of %d row(s), newest first</p></div></div>'
            '<div class="card-t">%s</div></section>'
            % (len(recent), total, events))

    if table == "error_log":
        # Keep the purpose-built, time-formatted error view and add a bounded
        # 24-hour occurrence overview. The aggregation uses the same already
        # redacted fields as the raw admin-only table.
        recent_24h = await d1_all(
            env,
            "SELECT ts,status,method,path,message,actor FROM error_log "
            "WHERE ts>=? ORDER BY ts DESC LIMIT 5000",
            now - 24 * 60 * 60 * 1000,
        )
        hourly = [0] * 24
        groups = {}
        for row in recent_24h or []:
            ts = int(row.get("ts") or 0)
            if 0 < ts < 10 ** 11:
                ts *= 1000
            age_hours = max(0, (now - ts) // (60 * 60 * 1000))
            if age_hours < 24:
                hourly[23 - int(age_hours)] += 1
            signature = (
                str(row.get("status") or ""),
                str(row.get("method") or "").upper(),
                str(row.get("path") or ""),
                str(row.get("message") or ""),
            )
            group = groups.setdefault(
                signature,
                {
                    "hours": [0] * 24,
                    "firstSeen": ts,
                    "lastSeen": ts,
                    # Accounts whose own sessions hit this exact failure, and
                    # how many occurrences carried no session at all. Grouping
                    # stays keyed on the failure itself so one user's traffic
                    # never splits a group; the affected users ride along.
                    "actors": {},
                    "anonymous": 0,
                },
            )
            group["firstSeen"] = min(group["firstSeen"] or ts, ts)
            group["lastSeen"] = max(group["lastSeen"] or ts, ts)
            actor = str(row.get("actor") or "").strip().lower()
            if actor:
                group["actors"][actor] = group["actors"].get(actor, 0) + 1
            else:
                group["anonymous"] += 1
            if age_hours < 24:
                group["hours"][23 - int(age_hours)] += 1

        def hour_label(index, count):
            hours_ago = 23 - index
            label = ("current hour" if hours_ago == 0
                     else "%d hours ago" % hours_ago)
            return "%s: %d error%s" % (label, count, "" if count == 1 else "s")

        chart = shell.hour_bars(
            hourly, ("24 hours ago", "18 h", "12 h", "6 h", "current hour"),
            hour_label)
        users_hit = set()
        anonymous_hits = 0
        for group in groups.values():
            users_hit.update(group["actors"])
            anonymous_hits += group["anonymous"]
        group_rows = []
        for (status, request_method, path, message), group in sorted(
                groups.items(),
                key=lambda item: (-sum(item[1]["hours"]), item[0]))[:25]:
            frequency = group["hours"]
            count = sum(frequency)
            related_users = _admin_error_related_users(
                group["actors"], group["anonymous"])
            status_text = str(status or "")
            status_css = (
                " s5" if status_text.startswith("5") else
                " s4" if status_text.startswith("4") else "")
            group_rows.append(
                '<tr><td><span class="code%s">%s</span></td>'
                '<td class="mono nowrap">%s %s</td>'
                "<td title=\"%s\"><span class=\"error-message\">%s</span>"
                "<div style=\"margin-top:6px\">%s</div></td>"
                "<td>%s</td><td>%s</td>"
                "%s"
                '<td>%s</td><td>%s</td>'
                '<td class="r"><b>%d</b></td>'
                '<td class="r"><div class="row-actions">%s%s</div></td></tr>'
                % (
                    status_css,
                    _html_escape(status or "error"),
                    _html_escape(request_method or "—"),
                    _html_escape(path or "—"),
                    _html_escape(message or "—"),
                    # A crash report earns its length: show all of it. The
                    # cell wraps, so the group stays readable at any size.
                    _html_escape(message or "—"),
                    _admin_error_copy_button(message or ""),
                    _admin_error_source_badge(request_method, path),
                    shell.sparkline(
                        frequency, "24-hour frequency: %d occurrence%s"
                        % (count, "" if count == 1 else "s")),
                    _admin_error_users_cell(
                        group["actors"], group["anonymous"]),
                    shell.time_html(group["firstSeen"], now),
                    shell.time_html(group["lastSeen"], now),
                    count,
                    _admin_error_bot_task_form(
                        status, request_method, path, message, related_users,
                        csrf_field, admin_query),
                    _admin_error_group_delete_form(
                        status, request_method, path, message,
                        csrf_field, admin_query),
                )
            )
        occurrences = len(recent_24h or [])
        analytics = (
            '<section class="card error-analytics" '
            'aria-labelledby="error-analytics-title" style="margin-bottom:16px">'
            '<div class="card-h"><div><h2 id="error-analytics-title">'
            'Previous 24 hours</h2><p>%d occurrence%s · %d distinct group%s · %d related '
            'user%s%s</p></div></div><div class="card-b">%s</div></section>'
            '<section class="card" style="margin-bottom:16px">'
            '<div class="card-h"><div><h2>Equivalent errors</h2><p>The '
            'busiest groups of identical status, method, path and message in '
            'the last 24 hours.</p></div></div><div class="card-t">%s</div>'
            '</section>'
            % (
                occurrences,
                "" if occurrences == 1 else "s",
                len(groups), "" if len(groups) == 1 else "s",
                len(users_hit), "" if len(users_hit) == 1 else "s",
                (" plus anonymous traffic" if anonymous_hits else ""),
                chart,
                (
                    "<table><thead><tr><th>Status</th><th>Request</th>"
                    "<th>Message</th><th>Source</th>"
                    "<th>24-hour frequency</th>"
                    "<th>Related users</th>"
                    "<th>First seen</th><th>Last seen</th>"
                    "<th class=\"r\">Count</th>"
                    "<th class=\"r\">Actions</th></tr></thead>"
                    "<tbody>" + "".join(group_rows) + "</tbody></table>"
                    if group_rows
                    else shell.empty("No errors in the previous 24 hours.")
                ),
            )
        )
        body = []
        for r in rows:
            status = str(r.get("status", "") or "")
            cls = (" s5" if status.startswith("5") else
                   " s4" if status.startswith("4") else "")
            body.append(
                "<tr%s>"
                % _admin_record_row_attrs(
                    admin_query, table, r.get("_rowid_", ""))
                + _admin_row_checkbox(r.get("_rowid_", ""))
                + '<td>%s</td>'
                "<td>%s</td>"
                '<td><span class="code%s">%s</span></td>'
                '<td class="mono nowrap">%s %s</td>'
                "<td><span class=\"error-message\">%s</span>"
                "<div style=\"margin-top:6px\">%s</div></td>"
                "%s<td class=\"mono muted\">%s</td>"
                '<td class="r"><div class="row-actions">%s%s</div></td></tr>'
                % (shell.time_html(r.get("ts", ""), now),
                   _admin_error_source_badge(
                       r.get("method", ""), r.get("path", "")),
                   cls, _html_escape(status or "—"),
                   _html_escape(r.get("method", "")), _html_escape(r.get("path", "")),
                   _html_escape(r.get("message", "")),
                   _admin_error_copy_button(r.get("message", "")),
                   _admin_error_row_user_cell(r.get("actor", "")),
                   _html_escape(r.get("ray", "") or "—"),
                   _admin_error_row_bot_task_button(
                       r.get("_rowid_", ""), admin_query),
                   _admin_error_row_delete_button(
                       r.get("_rowid_", ""), admin_query))
            )
        if not body:
            inner = shell.empty("No errors recorded yet.")
        else:
            inner = (_admin_bulk_form_open(table, csrf_field, admin_query)
                     + "<table><thead><tr>" + _admin_select_all_th()
                     + "<th>Time</th><th>Source</th><th>Status</th>"
                     "<th>Request</th><th>Message</th><th>Related user</th>"
                     "<th>CF-Ray</th>"
                     "<th class=\"r\">Actions</th></tr></thead><tbody>"
                     + "".join(body) + "</tbody></table></form>")
        return (
            shell.page_head(
                "Errors",
                "Worker, browser and desktop errors, grouped by status, "
                "method, path and message.", crumb="Reliability")
            + analytics
            + '<section class="card"><div class="card-h"><div><h2>Recent rows'
            '</h2><p>Newest %d of %d · open a row for its full record</p></div>'
            '</div><div class="card-t">%s</div></section>'
            % (min(len(rows), total), total, inner)
        )

    purge_allowed = _admin_purge_allowed(table)
    add_link = ""
    if not rows:
        return (prefix
                + '<section class="card"><div class="card-h"><div>'
                  '<h2 class="mono">%s</h2><p>0 rows%s</p></div></div>'
                  '<p class="empty">This table is empty.</p></section>'
                % (_html_escape(table), add_link))

    # Column order: union of keys, first row's order first. The synthetic
    # _rowid_ column drives row selection and is not displayed.
    columns = [c for c in rows[0].keys() if c != "_rowid_"]
    for r in rows:
        for k in r.keys():
            if k != "_rowid_" and k not in columns:
                columns.append(k)

    # Decrypt each row's `data` blob once, then promote the union of its
    # top-level JSON keys to real table columns so rows stay one line tall.
    # The data cell itself collapses to a marker with the pretty JSON in its
    # hover tooltip.
    decoded_rows = []
    for r in rows:
        decoded = None
        if isinstance(r.get("data"), str) and r.get("data"):
            decoded = await decrypt_row(env, r.get("data"))
        decoded_rows.append((
            r,
            _admin_redact_wallet_keys(decoded)
            if isinstance(decoded, dict) else None,
        ))
    json_cols = []
    for _r, decoded in decoded_rows:
        if decoded:
            for k in decoded:
                if k not in columns and k not in json_cols:
                    json_cols.append(k)
    json_cols = json_cols[:ADMIN_MAX_JSON_COLS]

    # Surface which columns the admin is looking at plaintext vs. ciphertext:
    # the `data` blob is AES-GCM encrypted at rest and only decrypted for this
    # view, so operators should not mistake a successful decrypt for the data
    # having been stored unencrypted.
    enc_notice = ""
    if "data" in columns:
        encrypted_total = sum(
            1 for r in rows if isinstance(r.get("data"), str) and r.get("data"))
        decrypted_ok = sum(
            1 for _r, decoded in decoded_rows if decoded is not None)
        plain_cols = [c for c in columns if c != "data"]
        sentence = (
            "<code>data</code> is stored AES-GCM encrypted and decrypted "
            "here for display (%d/%d row(s) decrypted)"
            % (decrypted_ok, encrypted_total)
        )
        if json_cols:
            sentence += ", expanded into " + ", ".join(
                "<code>%s</code>" % _html_escape(c) for c in json_cols)
        sentence += "."
        if plain_cols:
            sentence += " Column(s) " + ", ".join(
                "<code>%s</code>" % _html_escape(c) for c in plain_cols
            ) + " are stored as plaintext."
        enc_notice = (
            '<div class="banner info" style="margin:12px 18px 0">%s'
            '<span>%s</span></div>' % (shell.icon("lock"), sentence))
    else:
        enc_notice = (
            '<p class="note" style="margin:8px 18px 0">'
            'No encrypted columns in this table; all values are stored as '
            'plaintext.</p>'
        )

    body = []
    for r, decoded_data in decoded_rows:
        rid = r.get("_rowid_", "")
        cells = [_admin_row_checkbox(rid)] if purge_allowed else []
        for col in columns:
            value = r.get(col)
            if col == "data" and decoded_data is not None:
                cells.append(_admin_json_cell(decoded_data))
                continue
            cells.append(_admin_compact_cell(value))
        for col in json_cols:
            cells.append(_admin_compact_cell(
                None if decoded_data is None else decoded_data.get(col)))
        body.append(
            "<tr%s>%s</tr>"
            % (
                _admin_record_row_attrs(
                    admin_query, table, rid),
                "".join(cells),
            )
        )

    head = (_admin_select_all_th() if purge_allowed else "") + "".join(
        "<th>%s</th>" % _html_escape(c) for c in columns) + "".join(
        '<th class="jcol" title="from the data JSON">%s</th>' % _html_escape(c)
        for c in json_cols)
    table_markup = (
        '<table class="compact"><thead><tr>' + head + "</tr></thead><tbody>"
        + "".join(body) + "</tbody></table>"
    )
    if purge_allowed:
        table_markup = (
            _admin_bulk_form_open(table, csrf_field, admin_query)
            + table_markup
            + "</form>"
        )
    return (
        prefix
        + '<section class="card"><div class="card-h"><div>'
          '<h2 class="mono">%s</h2><p>%d row(s)%s · newest first · open a '
          'row for its full record</p></div>%s</div>'
        % (_html_escape(table), total,
           " (showing %d)" % row_limit if total > row_limit else "", add_link)
        + enc_notice
        + '<div class="card-t">' + table_markup + '</div></section>'
    )


def _render_admin_stats(stats):
    """The live network figures as four compact tiles."""
    shell = _admin_shell
    if not stats:
        return ""
    cells = []
    for key, label in (
            ("hosts", "live hosts"),
            ("clients", "chat clients"),
            ("repos", "catalog repos"),
            ("installs_24h", "installs (24 h)")):
        cells.append('<div class="mini"><b>%s</b><span>%s</span></div>' % (
            _html_escape(shell.format_int(stats.get(key, 0))), label))
    return '<div class="minis">' + "".join(cells) + "</div>"


async def _render_admin_overview(env, stats, admin_query="", now=None):
    """Overview: attention first, then growth and the live network.

    Every figure here is a bounded, indexed read or comes from the cached
    ``admin_stats`` snapshot. Anything that needs decrypting every account
    (signups per day, mirrors) stays on Product analytics, which runs in its
    own Durable Object shard for exactly that reason.
    """
    shell = _admin_shell
    now = _admin_now() if now is None else int(now)
    stats = stats or {}
    day = 24 * 60 * 60 * 1000

    async def safe_all(sql, *args):
        try:
            return [dict(row) for row in (await d1_all(env, sql, *args) or [])]
        except Exception:
            return None

    async def safe_count(sql, *args):
        rows = await safe_all(sql, *args)
        if not rows:
            return 0 if rows is not None else None
        return int(rows[0].get("n") or 0)

    recent = await safe_all(
        "SELECT ts FROM error_log WHERE ts>=? ORDER BY ts DESC LIMIT 5000",
        now - day) or []
    hourly = [0] * 24
    for row in recent:
        ts = int(row.get("ts") or 0)
        if 0 < ts < 10 ** 11:
            ts *= 1000
        age = max(0, (now - ts) // (60 * 60 * 1000))
        if age < 24:
            hourly[23 - int(age)] += 1
    errors_24h = int(stats.get("errors_24h", len(recent)) or 0)
    errors_prev = await safe_count(
        "SELECT COUNT(*) AS n FROM error_log WHERE ts>=? AND ts<?",
        now - 2 * day, now - day) or 0

    report_counts = await safe_all(
        "SELECT status, COUNT(*) AS n FROM bug_reports GROUP BY status")
    by_status = {str(r.get("status")): int(r.get("n") or 0)
                 for r in (report_counts or [])}
    crashes = await safe_count(
        "SELECT COUNT(*) AS n FROM bug_reports WHERE severity='crash' "
        "AND created_at>=?", now - 7 * day)
    latest_reports = await safe_all(
        "SELECT report_id, platform, app_version, severity, status, data, "
        "created_at FROM bug_reports ORDER BY created_at DESC LIMIT 5")

    origins = await safe_all(
        "SELECT signup_origin AS origin, COUNT(*) AS n FROM users "
        "GROUP BY signup_origin") or []
    origin_counts = {str(r.get("origin") or "unknown"): int(r.get("n") or 0)
                     for r in origins}
    account_rows = sum(origin_counts.values())

    campaign = await safe_all(
        "SELECT slug, title, status, participant_count, submission_count "
        "FROM hackathons ORDER BY created_at DESC LIMIT 1")
    campaign = campaign[0] if campaign else None
    referred = 0
    if campaign:
        referred = await safe_count(
            "SELECT COUNT(*) AS n FROM hackathon_participants "
            "WHERE hackathon_slug=? AND referrer_bi<>''",
            campaign.get("slug")) or 0

    views = {key: _admin_href(admin_query, view=key)
             for key in ("errors", "users", "bugs")}
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi(
            "Errors, last 24 h", shell.format_int(errors_24h),
            "vs. %s the day before · %s all time" % (
                shell.format_int(errors_prev),
                shell.format_int(stats.get("errors_total", 0))),
            change=shell.delta_pill(errors_24h, errors_prev,
                                    good_when_up=False),
            icon_name="alert", tone="bad" if errors_24h else "",
            href=views["errors"])
        + shell.kpi(
            "New bug reports", shell.format_int(by_status.get("new", 0)),
            ("%s triaged · %s closed" % (
                shell.format_int(by_status.get("triaged", 0)),
                shell.format_int(by_status.get("closed", 0))))
            if report_counts is not None else "Bug reports are not set up",
            change=(shell.pill("%d crash%s this week" % (
                crashes, "" if crashes == 1 else "es"), "bad")
                if crashes else ""),
            icon_name="bug", tone="bad" if by_status.get("new") else "",
            href=views["bugs"])
        + shell.kpi(
            "Account records", shell.format_int(account_rows),
            "Includes node reservations",
            icon_name="users", href=views["users"])
        + shell.kpi(
            "Catalog repositories", shell.format_int(stats.get("repos", 0)),
            "Public and private catalog records", icon_name="branch",
            href=shell.PRODUCT_ANALYSIS_HREF)
        + '</div>')

    segments = []
    for key, label, tone in (("organic", "Organic", "tone-accent"),
                             ("referral", "Referral", "tone-ok"),
                             ("job_flow", "Job flow", "tone-warn"),
                             ("unknown", "Unknown", "tone-second")):
        count = origin_counts.get(key, 0)
        segments.append(
            '<div class="segment"><span class="t"><span class="sw %s"></span>'
            '%s</span><b>%s</b><span class="bar %s%s"></span></div>' % (
                tone, label, shell.format_int(count), tone,
                "" if count else " tone-off"))
    growth = shell.card(
        '<div class="hero-chart"><div class="bigstat"><div class="n">%s</div>'
        '<div class="w">account records</div><div class="w">%s organic · %s '
        'through a referral</div></div><div>%s</div></div>'
        '<div class="segments">%s</div>' % (
            shell.format_int(account_rows),
            shell.format_int(origin_counts.get("organic", 0)),
            shell.format_int(origin_counts.get("referral", 0)),
            '<p class="note" style="margin:0">Signups and mirrors per day, '
            'clones and signup sources live in Product analytics, which '
            'decrypts every account record to count them exactly.</p>'
            '<p style="margin:12px 0 0"><a class="btn btn-sm" href="%s">Open '
            'product analytics</a></p>' % shell.PRODUCT_ANALYSIS_HREF,
            "".join(segments)),
        title="Growth", description="Accounts by signup origin",
        actions='<a class="more" href="%s">Product analytics%s</a>' % (
            shell.PRODUCT_ANALYSIS_HREF, shell.icon("chev")))

    peak = max(hourly) if hourly else 0
    errors_card = shell.card(
        shell.hour_bars(
            hourly, ("24 h ago", "12 h", "now"),
            lambda index, count: "%s: %d error%s" % (
                "current hour" if index == 23 else
                "%d hours ago" % (23 - index), count,
                "" if count == 1 else "s"))
        + ('<p class="note">Peak hour: %d error%s.</p>' % (
            peak, "" if peak == 1 else "s") if peak else
           '<p class="note">No errors in the last 24 hours.</p>'),
        title="Errors by hour", description="Worker, browser and desktop",
        actions='<a class="more" href="%s">Errors%s</a>' % (
            views["errors"], shell.icon("chev")))

    report_rows = []
    for report in latest_reports or []:
        record = None
        if report.get("data"):
            try:
                record = await decrypt_row(env, report.get("data"))
            except Exception:
                record = None
        record = record if isinstance(record, dict) else {}
        severity = str(report.get("severity") or "normal")
        report_rows.append(
            '<tr><td><span class="sev %s">%s</span></td>'
            '<td><span class="ellipsis">%s</span></td><td>%s</td>'
            '<td class="mono nowrap">%s</td><td>%s</td><td class="r">%s</td></tr>' % (
                _html_escape(severity), _html_escape(severity),
                _html_escape(record.get("message") or "—"),
                _html_escape(record.get("screen") or "—"),
                _html_escape(" ".join(filter(None, (
                    str(report.get("platform") or ""),
                    str(report.get("app_version") or "")))) or "—"),
                shell.pill(str(report.get("status") or "new"),
                           {"new": "info", "triaged": "warn"}.get(
                               str(report.get("status") or "new"), "")),
                shell.time_html(report.get("created_at"), now)))
    reports_card = shell.card(
        title="Latest bug reports",
        description="Sent from the report button in the mobile app",
        actions='<a class="more" href="%s">Open inbox%s</a>' % (
            views["bugs"], shell.icon("chev")),
        table=shell.table(
            "<th>Severity</th><th>Report</th><th>Screen</th><th>App</th>"
            "<th>Status</th><th class=\"r\">Received</th>",
            report_rows, "No bug reports yet."))

    if campaign:
        participants = int(campaign.get("participant_count") or 0)
        k_factor = (referred / participants) if participants else 0.0
        hackathon_card = shell.card(
            '<div class="gauge">%s<div class="n">%.2f</div>'
            '<div class="w">K-factor · referred joins per participant</div>'
            '<div class="form-row" style="justify-content:center">%s%s</div>'
            '</div>' % (
                shell.gauge(min(1.0, k_factor),
                            "K-factor %.2f of a 1.0 target" % k_factor),
                k_factor,
                shell.pill("%s participants" % shell.format_int(participants),
                           "line"),
                shell.pill("%s submissions" % shell.format_int(
                    campaign.get("submission_count") or 0), "line")),
            title="Hackathon", description="%s · %s" % (
                campaign.get("slug"), campaign.get("status")),
            actions='<a class="more" href="%s?slug=%s">Manage%s</a>' % (
                shell.subpage_base(env) + "/hackathon",
                quote(str(campaign.get("slug") or "")),
                shell.icon("chev")))
    else:
        hackathon_card = shell.card(
            shell.empty("No hackathon campaign exists yet."),
            title="Hackathon",
            actions='<a class="more" href="%s?slug=new">New campaign%s</a>'
            % (shell.subpage_base(env) + "/hackathon",
               shell.icon("chev")))

    network = (
        '<section class="card"><div class="card-h"><div><h2>Live network</h2>'
        '<p>Durable Object load, right now</p></div></div>%s</section>'
        % _render_admin_stats(stats))

    return (
        shell.page_head(
            "Overview",
            "Growth, support and reliability in one place. Each figure links "
            "to the page that owns it.")
        + kpis
        + '<div class="grid split">' + growth + errors_card + '</div>'
        + '<div class="grid split">' + reports_card
        + '<div class="stack">' + hackathon_card + network + '</div></div>'
    )


def _render_admin_repo_terms_flags(csrf_field="", admin_query=""):
    return (
        '<section id="repository-terms-flags" class="card" tabindex="-1">'
        '<div class="card-h"><div><h2>Repository Terms flags</h2>'
        '<p>Apply or clear the public policy-warning badge for a repository. '
        'The operator note is encrypted and remains admin-only.</p></div></div>'
        '<form class="card-b form-grid" method="post" action="%s">' %
        _admin_href(admin_query, action="set_repo_terms_flag",
                    view="controls") +
        csrf_field +
        '<div class="field"><label for="terms-owner">Owner</label>'
        '<input id="terms-owner" type="text" name="owner" placeholder="owner" '
        'pattern="[A-Za-z0-9._-]{1,100}" required></div>'
        '<div class="field"><label for="terms-repo">Repository</label>'
        '<input id="terms-repo" type="text" name="repo" '
        'placeholder="repository" pattern="[A-Za-z0-9._-]{1,100}" required>'
        '</div>'
        '<div class="field"><label for="terms-category">Category</label>'
        '<select id="terms-category" name="category" aria-label="Terms category">'
        '<option value="spam">Spam</option>'
        '<option value="malware">Malware</option>'
        '<option value="harassment">Harassment</option>'
        '<option value="illegal">Illegal content</option>'
        '<option value="other">Other</option></select></div>'
        '<div class="field"><span class="label">Action</span>'
        '<label class="switch"><input type="checkbox" name="clear" value="1">'
        '<span class="track"></span>Clear the flag</label></div>'
        '<div class="field wide"><label for="terms-note">Private operator note'
        '</label><input id="terms-note" type="text" name="note" maxlength="500" '
        'placeholder="Why this repository is flagged"></div>'
        '<div class="wide form-row" style="justify-content:flex-end">'
        '<button class="btn btn-primary" type="submit">Save flag</button></div>'
        '</form></section>'
    )


def _render_admin_signup_referral_code(
        state, csrf_field="", admin_query=""):
    state = state if isinstance(state, dict) else {}
    enabled = state.get("enabled") is True
    checked = " checked" if enabled else ""
    code = str(state.get("code") or "")
    code_detail = (
        '<p>Current one-use code: <code class="codebox">%s</code> · '
        'generation %s</p>' % (
            _html_escape(code),
            _html_escape(state.get("generation", 0)),
        )
        if enabled and code else
        '<p>The signup form is currently open without a referral code.</p>'
    )
    return (
        '<section id="signup-referral-code" class="card" tabindex="-1">'
        '<div class="card-h"><div><h2>Signup referral code</h2></div></div>'
        '<form class="setting" method="post" action="%s">' %
        _admin_href(admin_query, action="set_signup_referral_code",
                    view="controls") +
        csrf_field +
        '<div><h3>Require the referral code</h3><p>Require the rotating, '
        'one-use administrator code for password account signup. Changes '
        'take effect immediately.</p>' + code_detail + '</div>'
        '<div class="form-row"><label class="switch"><input type="checkbox" '
        'name="enabled" value="1"%s><span class="track"></span>'
        'Require referral code</label>' % checked +
        '<button class="btn btn-primary" type="submit">Save signup setting'
        '</button></div></form></section>'
    )


def _render_admin_nav(tables, active, counts=None, admin_query="", sort_records=False):
    """The Database section's table list, with row counts."""
    shell = _admin_shell
    counts = counts or {}
    if sort_records:
        tables = sorted(tables, key=lambda t: counts.get(t, 0), reverse=True)
    sort_key = "records" if sort_records else "name"
    toggle = (
        '<div class="seg" role="group" aria-label="Sort tables">'
        '<a href="%s"%s>Most rows</a><a href="%s"%s>A–Z</a></div>' % (
            _admin_href(admin_query, view="database", table=active,
                        sort="records"),
            ' aria-current="true"' if sort_records else "",
            _admin_href(admin_query, view="database", table=active,
                        sort="name"),
            "" if sort_records else ' aria-current="true"'))
    refresh = (
        '<a class="btn btn-sm" href="%s" title="Row counts are cached for '
        'a few minutes">%sRecount</a>' % (
            _admin_href(admin_query, view="database", table=active,
                        sort=sort_key, refresh_counts=1),
            shell.icon("refresh")))
    links = []
    for t in tables:
        restricted = t in ADMIN_HIDDEN_TABLES
        view = ADMIN_TABLE_VIEWS.get(t)
        # Tables with a purpose-built section open that section; the rest
        # open the generic, read-only table view. Sort state lives only in the
        # URL, so a table link that dropped it would reset the order.
        href = (_admin_href(admin_query, view=view) if view else
                _admin_href(admin_query, view="database", table=t,
                            sort=sort_key))
        n = counts.get(t)
        links.append('<a href="%s" class="%s%s"%s>%s<span class="mono">%s'
                     '</span><span class="c">%s</span></a>' % (
                         href, "on" if t == active else "",
                         " lock" if restricted else "",
                         ' aria-current="page"' if t == active else "",
                         shell.icon("lock") if restricted else "",
                         _html_escape("Error logs" if t == "error_log" else t),
                         "restricted" if restricted else
                         (shell.format_int(n) if n is not None else "")))
    return (
        '<nav class="card" aria-label="Tables"><div class="toolbar" '
        'style="padding:10px;justify-content:space-between">%s%s</div>'
        '<div class="tlist">%s</div></nav>' % (toggle, refresh, "".join(links)))


def _render_admin_controls(signup_referral_code, csrf_field="", admin_query=""):
    """Controls: the audited platform actions that are not a table view."""
    shell = _admin_shell
    ownership = (
        '<section class="card" id="ownership-transfer"><div class="card-h">'
        '<div><h2>Node ownership transfer</h2><p>Parks a pending transfer on '
        'the node\'s own account record. It only completes once that node\'s '
        'current owner approves the confirmation prompt on its own client.'
        '</p></div></div>'
        '<form class="card-b form-row" method="post" action="%s" '
        'onsubmit="return confirm(\'Request ownership transfer for this node?\')">'
        % _admin_href(admin_query, action="request_ownership", view="controls")
        + csrf_field +
        '<input type="text" name="target" placeholder="node to take (name)" '
        'autocomplete="off" required aria-label="Node to take" class="grow">'
        '<input type="text" name="owner" placeholder="new owner (account name)" '
        'autocomplete="off" required aria-label="New owner" class="grow">'
        '<button class="btn btn-primary" type="submit">Request ownership '
        'transfer</button></form></section>')
    custody = (
        '<section class="card" id="legacy-custody"><div class="card-h"><div>'
        '<h2>Legacy custody</h2></div></div>'
        '<form class="setting" method="post" action="%s" '
        'onsubmit="return confirm(\'Show legacy custody migration status?\')">'
        % _admin_href(admin_query, action="disburse", view="controls")
        + csrf_field +
        '<div><h3>Custody migration status</h3><p>Worker signing and automated '
        'sweeps are disabled. Historical encrypted deposit rows require an '
        'offline, balance-reconciled migration; this console cannot access '
        'or use wallet seeds.</p></div>'
        '<button class="btn" type="submit">%sLegacy custody status</button>'
        '</form></section>' % shell.icon("wallet"))
    return (
        shell.page_head(
            "Controls",
            "Sensitive platform actions. Every change is recorded in the "
            "audit log under your account name.", crumb="Platform")
        + '<div class="grid cols-2">'
        + _render_admin_signup_referral_code(
            signup_referral_code, csrf_field, admin_query)
        + ownership
        + _render_admin_repo_terms_flags(csrf_field, admin_query)
        + custody
        + '</div>'
    )


def _render_admin_database(tables, active, counts, table_html, admin_query="",
                           sort_records=False):
    """Database: the live table inventory beside the selected table."""
    shell = _admin_shell
    return (
        shell.page_head(
            "Database",
            "Every D1 table. Identity, custody and routing tables stay listed "
            "so the whole schema is accounted for, but their rows are "
            "restricted.", crumb="Platform")
        + '<div class="split-db">'
        + _render_admin_nav(tables, active, counts, admin_query, sort_records)
        + '<div class="stack">' + table_html + '</div></div>'
    )


def render_admin_html(view, content, banner="", admin_query="", console="",
                      theme="system", badges=None, account="", next_path="/",
                      subpages="/admin"):
    """A complete console page: the shared admin shell around one section.

    The console keeps the product analytics snippet it always loaded, and
    the one inline script the shell cannot replace (copying error messages).
    """
    shell = _admin_shell
    title = ADMIN_CONSOLE_VIEWS.get(view, "Admin")
    banner_html = shell.banner(banner, shell.banner_tone(banner)) if banner else ""
    return shell.render_page(
        title, banner_html + content, active=view, console=console,
        theme=theme, badges=badges, account=account, next_path=next_path,
        subpages=subpages,
        head_extra=(
            "<style>" + CONSOLE_STYLE + "</style>"
            "<script src=\"/analytics.js\"></script>"),
        body_extra=CONSOLE_SCRIPT)
