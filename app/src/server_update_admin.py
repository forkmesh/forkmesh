import secrets
from urllib.parse import parse_qs, quote, urlparse
from hidden_unicode import clean_query as clean_hidden_query

import deploy_approvals
import instance_legal
import repo_projects
import repo_short_urls
import server_update


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "update"
ADDRESS_VIEW = "repo-addresses"
LEGAL_VIEW = "legal-pages"
VIEWS = (VIEW, ADDRESS_VIEW, LEGAL_VIEW)
MAX_FORM_BYTES = 192 * 1024
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
    "projects_saved": ("Project addresses saved.", "ok"),
    "projects_invalid": ("Check the repository name, the project table and "
                         "the old owners.", "bad"),
    "owner_unknown": ("No account or organization has that name.", "bad"),
    "owner_invalid": ("Choose the account or organization that owns the "
                      "short addresses.", "bad"),
    "legal_saved": ("Legal page saved.", "ok"),
    "legal_invalid": ("Check the page: write its text for a page on this "
                      "site, or give a full http or https link.", "bad"),
    "legal_core": ("These pages are set on self-hosted instances only.",
                   "bad"),
    "auto_on": ("Merges to the source branch now update this server.", "ok"),
    "auto_off": ("Merges no longer update this server automatically.", "ok"),
    "auto_no_target": ("This server's source is not a repository on this "
                       "instance, so merges cannot update it.", "bad"),
    "auto_unprotected": ("Protect the source repository's default branch "
                         "first: anyone who can merge into it could "
                         "otherwise deploy code to this server.", "bad"),
    "csrf": ("Action blocked: reload the page and try again.", "bad"),
    "invalid": ("Action failed: unknown action.", "bad"),
}
ACTIONS = ("check_update", "start_update", "save_repo_paths",
           "save_repo_projects", "save_legal_page", "save_auto_update")
TRIGGER_LABELS = {"admin": "Update button", "merge": "Merged pull request"}
OUTCOME_LABELS = {"requested": "Update requested",
                  "queued": "Joined the update already waiting",
                  "write_failed": "The request could not be saved",
                  "unprotected": "Skipped: the source branch is not protected"}
LEGAL_MODES = (
    ("off", "Off: no link, the page is not found"),
    ("markdown", "A page on this site, written in Markdown"),
    ("url", "A link to a page on another site"),
)
LEGAL_HELP = {
    "terms": "The terms people accept when they sign up. When it is on, "
             "the sign-up page asks them to agree to it.",
    "privacy": "How this instance handles personal data.",
    "security": "How to report a vulnerability in this instance. When it is "
                "on, /.well-known/security.txt points to it.",
}


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
    if status["trigger"] and status["action"] == "update":
        rows.append(("Started by", _esc(TRIGGER_LABELS[status["trigger"]])))
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


def _daemon_notice(source):
    if source["daemon"] != "stale":
        return ""
    return (
        '<p class="hint">The git node program was built from older server '
        'code (%s) and keeps running until it is rebuilt: build '
        '<span class="mono">server/cmd/forkmesh-mirror-node</span> from the '
        'running commit and run the update with '
        '<span class="mono">FORKMESH_INSTANCE_DAEMON</span> '
        '(docs/INSTANCES.md).</p>' % _esc(
            _short(source["daemonBuiltCommit"]) or "unknown commit"))


async def _auto_update_card(env, csrf_field, admin_query, now):
    shell = _admin_shell  # noqa: F821
    source = server_update.build_source(env)
    try:
        wanted = await _self_update.current_target(env)
        setting = await _self_update.setting(env)
        last = await _self_update.last(env)
    except Exception:
        wanted, setting, last = None, {"auto": False}, {}
    rows = [("Source", '<span class="mono">%s</span>' % _esc(
        source["source"] or "unknown")),
        ("Branch", '<span class="mono">%s</span>' % _esc(
            source["ref"] or "unknown"))]
    if wanted:
        rows.append(("Repository", _esc(wanted[0] + "/" + wanted[1])))
    if last:
        rows.append(("Last merge", "%s: pull request #%d by %s, %s" % (
            shell.time_html(last["at"], now), last["pull"],
            _esc(last["actor"] or "unknown"),
            _esc(OUTCOME_LABELS[last["outcome"]]))))
    body = '<dl class="kv">%s</dl>' % "".join(
        "<dt>%s</dt><dd>%s</dd>" % (label, value) for label, value in rows)
    on = setting["auto"] and bool(wanted)
    if wanted:
        body += '<div style="margin-top:12px">%s</div>' % _form(
            "save_auto_update",
            "Turn off" if setting["auto"] else "Turn on", csrf_field,
            admin_query, extra='<input type="hidden" name="auto" value="%s">'
            % ("0" if setting["auto"] else "1"))
    else:
        body += ('<p class="hint">Point <span class="mono">sourceUrl</span> '
                 'in instance.json at a repository on this instance and run '
                 'one update first.</p>')
    return shell.card(
        title="Update when a pull request is merged",
        description="When a pull request is merged into the branch this "
        "server is built from, ask for the same update the button above "
        "starts. Only that repository and branch count; pushes, pull "
        "request branches and forks never do. The branch must be protected "
        "(merged only after an owner or admin approves), because whoever "
        "can merge into it deploys code to this server. Merges that land "
        "while an update runs are picked up by one more update afterwards.",
        actions=shell.pill("On" if on else "Off", "ok" if on else ""),
        body=body)


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
                     "running here.", body=_commit_rows(current, now)
                     + _daemon_notice(server_update.build_source(env)),
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
        update += await _auto_update_card(env, csrf_field, admin_query, now)
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
            shell.switch("canonical", "Show repositories without the owner "
                         "(single-organization instance)",
                         setting["canonical"]),
            _esc(setting["owner"])))
    full = (
        "https://%s/%s/&lt;repository&gt; redirects to "
        "https://%s/&lt;repository&gt;" % (host, owner, host)
        if setting["canonical"] else
        "https://%s/%s/&lt;repository&gt; keeps working" % (host, owner))
    examples = (
        '<dl class="kv">'
        "<dt>Repository</dt><dd class=\"mono\">https://%s/&lt;repository&gt;"
        "</dd>"
        "<dt>Folder</dt><dd class=\"mono\">https://%s/&lt;repository&gt;/"
        "&lt;folder&gt;</dd>"
        "<dt>Clone</dt><dd class=\"mono\">git clone https://%s/"
        "&lt;repository&gt;.git</dd>"
        "<dt>Full address</dt><dd class=\"mono\">%s</dd></dl>" % (
            host, host, host, full))
    return shell.page_head(
        "Repository addresses",
        "Let the repositories of one account or organization open without "
        "the owner in the address.", crumb="Platform") + shell.card(
        title="Short addresses",
        description="When on, /<repository> and /<repository>/<folder> open "
        "that owner's repository. Names of accounts, organizations and "
        "site pages keep their own addresses. With the second switch the "
        "site links, clone URLs and the address bar show only "
        "/<repository> for that owner, and the full address redirects "
        "there; Git and API addresses keep working at both.",
        body=form, table=examples) + await render_projects(
            env, request, csrf_field, admin_query)


async def render_projects(env, request, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    setting = repo_projects.parse(
        await _settings_doc(env, repo_projects.SETTING_KEY))
    host = _esc(urlparse(request.url).netloc or "example.com")
    repo = _esc(setting["repo"] or "<repository>")
    form = (
        '<form method="post" action="%s" class="stack">%s'
        '<input type="hidden" name="action" value="save_repo_projects">'
        '<label class="form-row"><span>Repository</span>'
        '<input type="text" name="repo" value="%s" class="grow" '
        'placeholder="repository with one folder per project" '
        'autocomplete="off" aria-label="Repository"></label>'
        '<label class="stack"><span>Projects: one line per folder, '
        'tab-separated: folder, address name, old names separated by '
        'commas. A folder of - sends its names to the repository.</span>'
        '<textarea name="table" rows="12" class="mono" spellcheck="false" '
        'aria-label="Project table">%s</textarea></label>'
        '<label class="form-row"><span>Old owners</span>'
        '<input type="text" name="legacy" value="%s" class="grow" '
        'placeholder="owners whose old repository addresses redirect" '
        'autocomplete="off" aria-label="Old owners"></label>'
        '<div><button type="submit" class="btn btn-primary">Save</button>'
        '</div></form>' % (
            _admin_href(admin_query, view=ADDRESS_VIEW),  # noqa: F821
            csrf_field, _esc(setting["repo"]), _esc(setting["table"]),
            _esc(", ".join(setting["legacy"]))))
    examples = (
        '<dl class="kv">'
        "<dt>Project</dt><dd class=\"mono\">https://%s/%s/&lt;Project&gt;</dd>"
        "<dt>File</dt><dd class=\"mono\">https://%s/%s/&lt;Project&gt;/blob/"
        "&lt;branch&gt;/&lt;path&gt;</dd>"
        "<dt>History</dt><dd class=\"mono\">https://%s/%s/&lt;Project&gt;/"
        "commits</dd>"
        "<dt>Issues</dt><dd class=\"mono\">https://%s/%s/&lt;Project&gt;/"
        "issues (label &lt;folder&gt;)</dd>"
        "<dt>Old address</dt><dd class=\"mono\">https://%s/&lt;old owner&gt;/"
        "&lt;old name&gt; redirects to the project</dd></dl>" % (
            host, repo, host, repo, host, repo, host, repo, host))
    return shell.card(
        title="Project addresses",
        description="For a repository that keeps one project per folder: "
        "give each folder its own address under the short repository "
        "address, and send old repository addresses to it. Needs short "
        "addresses on.",
        body=form, table=examples)


def _legal_form(page, entry, csrf_field, admin_query):
    options = "".join(
        '<option value="%s"%s>%s</option>' % (
            mode, " selected" if entry["mode"] == mode else "", _esc(label))
        for mode, label in LEGAL_MODES)
    contact = ""
    if page == "security":
        contact = (
            '<label class="form-row"><span>Security contact</span>'
            '<input type="text" name="contact" value="%s" class="grow" '
            'placeholder="security@example.com or https://..." '
            'autocomplete="off" aria-label="Security contact"></label>'
            % _esc(entry.get("contact")))
    return (
        '<form method="post" action="%s" class="stack">%s'
        '<input type="hidden" name="action" value="save_legal_page">'
        '<input type="hidden" name="page" value="%s">'
        '<label class="form-row"><span>Show</span>'
        '<select name="mode" aria-label="%s">%s</select></label>'
        '<label class="stack"><span>Page text, in Markdown (used when the '
        'page is on this site)</span>'
        '<textarea name="markdown" rows="12" class="mono" spellcheck="true" '
        'aria-label="%s text">%s</textarea></label>'
        '<label class="form-row"><span>Link</span>'
        '<input type="url" name="url" value="%s" class="grow" '
        'placeholder="https://example.com/%s" autocomplete="off" '
        'aria-label="%s link"></label>%s'
        '<div><button type="submit" class="btn btn-primary">Save</button>'
        '</div></form>' % (
            _admin_href(admin_query, view=LEGAL_VIEW),  # noqa: F821
            csrf_field, page, _esc(instance_legal.TITLES[page]), options,
            _esc(instance_legal.TITLES[page]), _esc(entry["markdown"]),
            _esc(entry["url"]), page, _esc(instance_legal.TITLES[page]),
            contact))


async def render_legal(env, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    head = shell.page_head(
        "Legal pages",
        "Choose the Terms, Privacy and Security pages this instance links "
        "to.", crumb="Platform")
    if not instance_branded(env):  # noqa: F821
        return head + shell.card(
            title="ForkMesh's own pages",
            description="This server serves forkmesh.com's own Terms, "
            "Privacy and Security pages. These settings are for self-hosted "
            "instances.")
    setting = instance_legal.parse(
        await _settings_doc(env, instance_legal.SETTING_KEY))
    cards = []
    for page in instance_legal.IDS:
        entry = setting[page]
        on = instance_legal.enabled(setting, page)
        target = instance_legal.href(setting, page)
        actions = shell.pill("On" if on else "Off", "ok" if on else "")
        if on:
            actions += ' <a class="btn" href="%s" rel="noopener">Open</a>' % (
                _esc(target))
        cards.append(shell.card(
            title=instance_legal.TITLES[page],
            description=LEGAL_HELP[page] + " Off by default: the link is "
            "hidden everywhere and %s is not found." %
            instance_legal.PATHS[page],
            actions=actions,
            body=_legal_form(page, entry, csrf_field, admin_query)))
    return head + "".join(cards)


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
    if view == LEGAL_VIEW:
        title = "Legal pages"
        content = await render_legal(env, csrf_field, admin_query)
    elif view == ADDRESS_VIEW:
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
        subpages=shell.subpage_base(env),
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
                                        _first(form, "owner"),
                                        _first(form, "canonical") == "1")
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


async def _save_projects(env, form, short):
    try:
        setting = repo_projects.clean(_first(form, "repo"),
                                      _first(form, "table"),
                                      _first(form, "legacy"))
    except repo_projects.SettingError:
        return "projects_invalid", None
    if setting["repo"] and not short["owner"]:
        return "owner_invalid", None
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at",
        repo_projects.SETTING_KEY, repo_projects.dump(setting), _now())
    repo_projects.forget()
    return "projects_saved", {"repo": setting["repo"],
                              "legacy": setting["legacy"]}


async def _save_legal(env, form):
    if not instance_branded(env):  # noqa: F821
        return "legal_core", None
    page = _first(form, "page")
    try:
        entry = instance_legal.clean(
            page, _first(form, "mode"), _first(form, "markdown"),
            _first(form, "url"), _first(form, "contact"))
    except instance_legal.SettingError:
        return "legal_invalid", ({"page": page}
                                 if page in instance_legal.IDS else None)
    setting = instance_legal.with_page(instance_legal.parse(
        await _settings_doc(env, instance_legal.SETTING_KEY)), page, entry)
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at",
        instance_legal.SETTING_KEY, instance_legal.dump(setting), _now())
    instance_legal.forget()
    return "legal_saved", {"page": page, "mode": entry["mode"]}


async def _save_auto_update(env, form):
    if server_update.mode(env) != "instance":
        return "unavailable", {}
    auto = _first(form, "auto") == "1"
    wanted = await _self_update.current_target(env)
    if auto and not wanted:
        return "auto_no_target", {}
    if auto and not await _self_update.source_protected(env, *wanted):
        return "auto_unprotected", {}
    await _self_update.save(env, auto)
    return ("auto_on" if auto else "auto_off"), {
        "auto": auto, "repo": "/".join(wanted[:2]) if wanted else "",
        "branch": wanted[2] if wanted else ""}


async def _handle_post(env, request, view, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = clean_hidden_query(parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True))
    target = view if view in VIEWS else VIEW

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
    elif action == "save_repo_projects":
        short = repo_short_urls.parse(
            await _settings_doc(env, repo_short_urls.SETTING_KEY))
        notice, saved = await _save_projects(env, form, short)
        if saved is not None:
            details = saved
    elif action == "save_legal_page":
        notice, saved = await _save_legal(env, form)
        if saved is not None:
            details = saved
    elif action == "save_auto_update":
        notice, details = await _save_auto_update(env, form)
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
