import secrets
from urllib.parse import parse_qs, quote, urlparse

import deploy_approvals
import repo_short_urls
import server_update


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "update"
ADDRESS_VIEW = "repo-addresses"
VIEWS = (VIEW, ADDRESS_VIEW)
MAX_FORM_BYTES = 2048
RECENT_CORE_UPDATES = 5
NOTICES = {
    "update_started": ("Update requested. This page refreshes while it runs.",
                       "ok"),
    "check_started": ("Checking the ForkMesh source for a newer version.",
                      "ok"),
    "busy": ("An update or check is already running.", "warn"),
    "unconfirmed": ("Tick the box to confirm the update.", "warn"),
    "unavailable": ("Updates from this page are not set up on this server.",
                    "bad"),
    "write_failed": ("The update request could not be saved on this server.",
                     "bad"),
    "addresses_saved": ("Repository addresses saved.", "ok"),
    "owner_unknown": ("No account or organization has that name.", "bad"),
    "owner_invalid": ("Choose the account or organization that owns the "
                      "short addresses.", "bad"),
    "csrf": ("Action blocked: reload the page and try again.", "bad"),
    "invalid": ("Action failed: unknown action.", "bad"),
}
ACTIONS = ("check_update", "start_update", "save_repo_paths")


def _first(params, key):
    return str((params.get(key) or [""])[0] or "")


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + quote(path, safe="/-_.~") if path else ""


def _now():
    return int(Date.now())  # noqa: F821


def _html(body):
    return Response(body, status=200, headers={  # noqa: F821
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


def _redirect(location):
    return Response("", status=303, headers={  # noqa: F821
        "location": location, "cache-control": "no-store"})


def _esc(value):
    return _html_escape(value)  # noqa: F821


def _short(commit_id):
    return str(commit_id or "")[:12]


def _commit_rows(info, now):
    shell = _admin_shell  # noqa: F821
    rows = [
        ("Version", _esc(info.get("version") or "unknown")),
        ("Commit", '<span class="mono" title="%s">%s</span>' % (
            _esc(info.get("commit")), _esc(_short(info.get("commit"))))
         if info.get("commit") else '<span class="muted">unknown</span>'),
        ("Committed", shell.time_html(info.get("time"), now)),
        ("Message", _esc(info.get("subject")) or
         '<span class="muted">unknown</span>'),
    ]
    if info.get("builtAt"):
        rows.append(("Built", shell.time_html(info["builtAt"], now)))
    return '<dl class="kv">%s</dl>' % "".join(
        "<dt>%s</dt><dd>%s</dd>" % (label, value) for label, value in rows)


def _form(action, label, csrf_field, admin_query, view=VIEW, primary=False,
          confirm="", extra=""):
    check = (
        '<label class="hint"><input type="checkbox" name="confirm" '
        'value="yes" required> %s</label> ' % _esc(confirm)
        if confirm else "")
    return (
        '<form method="post" action="%s" class="form-row">%s'
        '<input type="hidden" name="action" value="%s">%s%s'
        '<button type="submit" class="btn%s">%s</button></form>' % (
            _admin_href(admin_query, view=view),  # noqa: F821
            csrf_field, action, extra, check,
            " btn-primary" if primary else "", _esc(label)))


def _freshness_pill(state):
    shell = _admin_shell  # noqa: F821
    if state == "current":
        return shell.pill("Up to date", "ok")
    if state == "behind":
        return shell.pill("Update available", "warn")
    return shell.pill("Not checked yet", "info")


def _instance_progress(status, now):
    shell = _admin_shell  # noqa: F821
    state = status["state"]
    if status["requested"]:
        label = shell.pill("Starting", "info")
    elif state == "updating":
        label = shell.pill("Updating", "info")
    elif state == "checking":
        label = shell.pill("Checking", "info")
    elif state == "done":
        label = shell.pill("Updated", "ok")
    elif state == "failed":
        label = shell.pill("Failed", "bad")
    else:
        label = shell.pill("Idle", "")
    rows = [("Status", label)]
    if status["step"] and state == "updating":
        rows.append(("Step", _esc(server_update.step_label(status["step"]))))
    if status["startedAt"]:
        rows.append(("Started", shell.time_html(status["startedAt"], now)))
    if status["finishedAt"]:
        rows.append(("Finished", shell.time_html(status["finishedAt"], now)))
    if status["fromCommit"] and status["action"] == "update":
        moved = _short(status["fromCommit"])
        if status["toCommit"]:
            moved += " → " + _short(status["toCommit"])
        rows.append(("Commits", '<span class="mono">%s</span>' % _esc(moved)))
    if status["lastError"] and state == "failed":
        rows.append(("Error", _esc(status["lastError"])))
    return '<dl class="kv">%s</dl>' % "".join(
        "<dt>%s</dt><dd>%s</dd>" % (label, value) for label, value in rows)


def _core_table(rows, now):
    shell = _admin_shell  # noqa: F821
    tones = {"deployed": "ok", "failed": "bad", "skipped": "warn",
             "pending": "info", "deploying": "info"}
    body = []
    for row in rows:
        nodes = ", ".join(
            "%s: %s" % (n["node"], n["status"]) for n in row["nodes"])
        body.append(
            "<tr><td>%s</td><td>%s</td><td class=\"mono\">%s</td>"
            "<td>%s</td><td>%s</td></tr>" % (
                shell.time_html(row["createdAt"], now),
                _esc(row["actor"]), _esc(_short(row["commit"])),
                shell.pill(row["status"], tones.get(row["status"], "")),
                _esc("; ".join(p for p in (nodes, row["message"]) if p))))
    return shell.table(
        "<th>Requested</th><th>By</th><th>Commit</th><th>Status</th>"
        "<th>Cores</th>", body, "No update has been requested here yet.")


async def _core_rows(env):
    rows = await d1_all(  # noqa: F821
        env,
        "SELECT id,base_after,actor,status,message,created_at FROM "
        "deploy_approvals WHERE kind=? ORDER BY id DESC LIMIT ?",
        server_update.CORE_KIND, RECENT_CORE_UPDATES)
    out = []
    for row in rows or []:
        nodes = await d1_all(  # noqa: F821
            env,
            "SELECT node,status FROM deploy_approval_nodes WHERE "
            "approval_id=? ORDER BY node", row["id"])
        out.append({
            "commit": str(row.get("base_after") or ""),
            "actor": str(row.get("actor") or ""),
            "status": str(row.get("status") or ""),
            "message": str(row.get("message") or "")[:300],
            "createdAt": int(row.get("created_at") or 0),
            "nodes": [{"node": str(n.get("node") or ""),
                       "status": str(n.get("status") or "")}
                      for n in nodes or []],
        })
    return out


async def _core_upstream(env):
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?",
            server_update.CORE_UPSTREAM_KEY)
    except Exception:
        row = None
    return server_update.core_upstream((row or {}).get("doc") or "")


async def _settings_doc(env, key):
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?", key)
    except Exception:
        row = None
    return (row or {}).get("doc") or ""


async def render_update(env, csrf_field, admin_query, now):
    shell = _admin_shell  # noqa: F821
    kind = server_update.mode(env)
    current = server_update.running(env)
    busy = False
    if kind == "instance":
        status = server_update.instance_status(env)
        busy = server_update.busy(status)
        upstream = dict(status["upstream"], checkedAt=status["checkedAt"])
    elif kind == "core":
        status = None
        upstream = await _core_upstream(env)
        core_rows = await _core_rows(env)
        busy = any(r["status"] in server_update.CORE_OPEN for r in core_rows)
    else:
        status = None
        upstream = {}
    freshness = server_update.freshness(current, upstream)
    latest_body = (
        _commit_rows(upstream, now) if upstream.get("commit") else
        shell.empty("The newest ForkMesh version has not been looked up "
                    "yet."))
    checked = ""
    if upstream.get("checkedAt"):
        checked = "Checked %s." % shell.relative_label(
            upstream["checkedAt"], now)
    check_button = (
        _form("check_update", "Check now", csrf_field, admin_query)
        if kind == "instance" and not busy else "")
    cards = (
        '<div class="grid cols-2">'
        + shell.card(title="This server", description="The ForkMesh build "
                     "running here.", body=_commit_rows(current, now),
                     actions=_freshness_pill(freshness))
        + shell.card(title="Latest available",
                     description=checked or "The newest commit on the "
                     "ForkMesh source this server follows.",
                     body=latest_body, actions=check_button)
        + "</div>")
    if kind == "instance":
        update_body = _instance_progress(status, now)
        if not busy:
            update_body += '<div style="margin-top:12px">%s</div>' % _form(
                "start_update", "Update ForkMesh", csrf_field, admin_query,
                primary=True, confirm="Update and restart now")
        update = shell.card(
            title="Update",
            description="Pulls the latest ForkMesh from its source, rebuilds "
            "the app, migrates the database and restarts only what changed. "
            "Your data, settings and accounts stay. The site may be "
            "unavailable for a minute while the app restarts.",
            body=update_body)
    elif kind == "core":
        button = "" if busy else _form(
            "start_update", "Update ForkMesh", csrf_field, admin_query,
            primary=True, confirm="Deploy main to every core")
        update = shell.card(
            title="Update",
            description="Asks the cores to deploy the newest commit on main. "
            "They build and activate it one core at a time, migrating the "
            "database once, and stop at the first failure.",
            body=button, table=_core_table(core_rows, now))
    else:
        update = shell.card(
            title="Update",
            description="Updates from this page are not set up on this "
            "server.",
            body="<p>On a self-hosted instance, run <span class=\"mono\">"
            "bash install-instance.sh --update</span> once on the server; "
            "after that this button updates it. On the ForkMesh cores, set "
            "<span class=\"mono\">[self_deploy] admin_updates = true</span> "
            "in <span class=\"mono\">deploy/fleet.toml</span> and roll the "
            "node files.</p>")
    return busy, shell.page_head(
        "Update ForkMesh",
        "See which ForkMesh version this server runs and update it to the "
        "latest.", crumb="Platform") + cards + update


async def render_addresses(env, request, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    setting = repo_short_urls.parse(
        await _settings_doc(env, repo_short_urls.SETTING_KEY))
    host = _esc(urlparse(request.url).netloc or "example.com")
    owner = _esc(setting["owner"] or "<owner>")
    form = (
        '<form method="post" action="%s" class="stack">%s'
        '<input type="hidden" name="action" value="save_repo_paths">'
        '<div>%s</div>'
        '<label class="form-row"><span>Owner of the short addresses</span>'
        '<input type="text" name="owner" value="%s" class="grow" '
        'placeholder="account or organization" autocomplete="off" '
        'aria-label="Owner of the short addresses"></label>'
        '<div><button type="submit" class="btn btn-primary">Save</button>'
        '</div></form>' % (
            _admin_href(admin_query, view=ADDRESS_VIEW),  # noqa: F821
            csrf_field,
            shell.switch("short", "Serve repositories at /<repository>",
                         setting["short"]),
            _esc(setting["owner"])))
    examples = (
        '<dl class="kv">'
        "<dt>Repository</dt><dd class=\"mono\">https://%s/&lt;repository&gt;"
        "</dd>"
        "<dt>Folder</dt><dd class=\"mono\">https://%s/&lt;repository&gt;/"
        "&lt;folder&gt;</dd>"
        "<dt>Clone</dt><dd class=\"mono\">git clone https://%s/"
        "&lt;repository&gt;.git</dd>"
        "<dt>Full address</dt><dd class=\"mono\">https://%s/%s/"
        "&lt;repository&gt; keeps working</dd></dl>" % (
            host, host, host, host, owner))
    return shell.page_head(
        "Repository addresses",
        "Let the repositories of one account or organization open without "
        "the owner in the address.", crumb="Platform") + shell.card(
        title="Short addresses",
        description="When on, /<repository> and /<repository>/<folder> open "
        "that owner's repository. Names of accounts, organizations and "
        "site pages keep their own addresses.",
        body=form, table=examples)


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    view = _first(params, "view")
    if method_name(request) == "POST":  # noqa: F821
        return await _handle_post(env, request, view, admin_query, account)
    shell = _admin_shell  # noqa: F821
    notice = NOTICES.get(_first(params, "notice"))
    banner = shell.banner(notice[0], notice[1]) if notice else ""
    now = _now()
    head_extra = ""
    if view == ADDRESS_VIEW:
        title = "Repository addresses"
        content = await render_addresses(env, request, csrf_field,
                                         admin_query)
    else:
        title = "Update ForkMesh"
        busy, content = await render_update(env, csrf_field, admin_query,
                                            now)
        if busy:
            head_extra = '<meta http-equiv="refresh" content="5">'
    url = urlparse(request.url)
    return _html(shell.render_page(
        title, banner + content, active=view, console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        next_path=url.path + (("?" + url.query) if url.query else ""),
        head_extra=head_extra))


async def _start_core_update(env, account):
    open_row = await d1_first(  # noqa: F821
        env,
        "SELECT id FROM deploy_approvals WHERE kind=? AND status IN (?,?) "
        "LIMIT 1", server_update.CORE_KIND, *server_update.CORE_OPEN)
    if open_row:
        return "busy"
    owner, repo = deploy_approvals.repository(env).split("/", 1)
    now = _now()
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO deploy_approvals (request_id,kind,owner,repo,target,"
        "base_before,head,base_after,public_key,signature,signed_at,actor,"
        "status,created_at,updated_at) VALUES (?,?,?,?,?,'','','','','',?,?,"
        "'pending',?,?)",
        "admin:%d:%s" % (now, secrets.token_hex(8)), server_update.CORE_KIND,
        owner, repo, "main", now, account, now, now)
    return "update_started"


async def _save_addresses(env, form):
    try:
        setting = repo_short_urls.clean(_first(form, "short") == "1",
                                        _first(form, "owner"))
    except repo_short_urls.SettingError:
        return "owner_invalid", None
    if setting["owner"]:
        _, account_rec = await _account_row(env, setting["owner"])  # noqa: F821
        _, org_rec = await _org_row(env, setting["owner"])  # noqa: F821
        if not account_rec and not org_rec:
            return "owner_unknown", None
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at",
        repo_short_urls.SETTING_KEY, repo_short_urls.dump(setting), _now())
    repo_short_urls.forget()
    return "addresses_saved", setting


async def _handle_post(env, request, view, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True)
    target = ADDRESS_VIEW if view == ADDRESS_VIEW else VIEW

    def location(notice):
        return _console_path(env) + _admin_href(  # noqa: F821
            admin_query, view=target, notice=notice).replace("&amp;", "&")

    if not _admin_csrf_ok(env, form):  # noqa: F821
        return _redirect(location("csrf"))
    action = _first(form, "action")
    if action not in ACTIONS:
        return _redirect(location("invalid"))
    details = {}
    if action == "save_repo_paths":
        notice, setting = await _save_addresses(env, form)
        if setting is not None:
            details = setting
    elif action == "start_update" and _first(form, "confirm") != "yes":
        return _redirect(location("unconfirmed"))
    else:
        kind = server_update.mode(env)
        if kind == "instance":
            if server_update.busy(server_update.instance_status(env)):
                notice = "busy"
            elif server_update.request_instance(
                    env, "update" if action == "start_update" else "check"):
                notice = ("update_started" if action == "start_update"
                          else "check_started")
            else:
                notice = "write_failed"
        elif kind == "core" and action == "start_update":
            notice = await _start_core_update(env, account)
        else:
            notice = "unavailable"
        details = {"mode": kind}
    await _audit_sensitive_action(  # noqa: F821
        env, account, "admin.console_" + action, "server", target,
        "success" if NOTICES[notice][1] == "ok" else "refused",
        dict(details, notice=notice))
    return _redirect(location(notice))
