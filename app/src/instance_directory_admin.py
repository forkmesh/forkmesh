from urllib.parse import parse_qs, quote, urlparse

import instance_directory
from hidden_unicode import clean_query as clean_hidden_query


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "instances"
PROBE_TIMEOUT_SECONDS = 8
MAX_FORM_BYTES = 2048
ACTIONS = ("hide_instance", "show_instance", "delete_instance")
NOTICES = {
    "hidden": ("Hidden from the homepage and the World.", "ok"),
    "shown": ("Shown again while it answers.", "ok"),
    "deleted": ("Deleted from the instance directory.", "ok"),
    "unconfirmed": ("Tick the box to confirm the delete.", "warn"),
    "missing": ("That instance is no longer in the directory.", "warn"),
    "csrf": ("Action blocked: reload the page and try again.", "bad"),
    "invalid": ("Action failed: unknown action.", "bad"),
}


def _first(params, key):
    return str((params.get(key) or [""])[0] or "")


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + quote(path, safe="/-_.~") if path else ""


class EnvRuntime:
    def __init__(self, env):
        self.env = env

    def now(self):
        return int(Date.now())  # noqa: F821

    async def d1_all(self, sql, *args):
        return await d1_all(self.env, sql, *args)  # noqa: F821

    async def d1_first(self, sql, *args):
        return await d1_first(self.env, sql, *args)  # noqa: F821

    async def d1_run(self, sql, *args):
        return await d1_run(self.env, sql, *args)  # noqa: F821

    async def probe(self, url):
        try:
            response = await js_fetch_with_timeout(  # noqa: F821
                url, {"method": "GET", "redirect": "manual"},
                PROBE_TIMEOUT_SECONDS)
        except Exception:
            return False
        return int(getattr(response, "status", 0) or 0) == 200

    async def ping_admins(self, ping):
        names = await _operator_alerts._operator_alert_admins(  # noqa: F821
            self.env)
        delivered = 0
        for name in names or []:
            try:
                if await enqueue_notification(  # noqa: F821
                        self.env, name, "operational_alert", ping["title"],
                        body=ping["body"],
                        href=(_console_path(self.env) or "/admin")
                        + "?view=" + VIEW,
                        source="instance-directory", dedupe=ping["dedupe"],
                        ts=self.now()):
                    delivered += 1
            except Exception:
                continue
        return delivered


async def health_cron(env):
    return await instance_directory.check_health(EnvRuntime(env))


def _html(body):
    return Response(body, status=200, headers={  # noqa: F821
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


def _redirect(location):
    return Response("", status=303, headers={  # noqa: F821
        "location": location, "cache-control": "no-store"})


def _status(row, now):
    shell = _admin_shell  # noqa: F821
    if int(row.get("hidden") or 0):
        return shell.pill("Hidden by an admin", "warn")
    if instance_directory.offline(row):
        return "%s since %s" % (
            shell.pill("Offline, not shown", "bad"),
            shell.time_html(row.get("offline_since"), now))
    if not int(row.get("checked_at") or 0):
        return shell.pill("Not checked yet", "info")
    if int(row.get("failures") or 0):
        return shell.pill("Missed one check", "warn")
    return shell.pill("Online", "ok")


def _shown_on(row):
    places = []
    if int(row.get("listed") or 0):
        places.append("Homepage")
    if int(row.get("world") or 0):
        slot = int(row.get("slot", -1))
        places.append("World hexagon %d" % (slot + 1) if slot >= 0
                      else "World (no free hexagon)")
    return ", ".join(places) or "Nowhere"


def _source(row, now):
    if row.get("public_key"):
        return "Joined from its own console, announced %s" % (
            _admin_shell.time_html(row.get("announced_at"), now))  # noqa: F821
    return "Started from /start"


def _action_form(action, row, label, csrf_field, admin_query, danger=False):
    confirm = (
        '<label class="hint"><input type="checkbox" name="confirm" '
        'value="yes" required> sure</label> ' if danger else "")
    return (
        '<form method="post" action="%s" style="display:inline">%s'
        '<input type="hidden" name="action" value="%s">'
        '<input type="hidden" name="id" value="%s">%s'
        '<button type="submit" class="btn btn-sm%s">%s</button></form>' % (
            _admin_href(admin_query, view=VIEW),  # noqa: F821
            csrf_field, action, _html_escape(row["id"]),  # noqa: F821
            confirm, " btn-danger" if danger else "", label))


def render(rows, csrf_field, admin_query, now):
    shell = _admin_shell  # noqa: F821
    body = []
    for row in rows:
        toggle = (
            _action_form("show_instance", row, "Show", csrf_field, admin_query)
            if int(row.get("hidden") or 0) else
            _action_form("hide_instance", row, "Hide", csrf_field, admin_query))
        body.append(
            "<tr><td><strong>%s</strong><br>"
            '<a class="muted mono" href="%s" rel="noopener" '
            'target="_blank">%s</a><br><span class="muted">%s</span></td>'
            "<td>%s</td><td>%s</td><td>%s</td><td>%s</td>"
            '<td class="r">%s %s</td></tr>' % (
                _html_escape(row.get("name") or ""),  # noqa: F821
                _html_escape(row.get("url") or ""),  # noqa: F821
                _html_escape(row.get("domain") or ""),  # noqa: F821
                _source(row, now),
                _html_escape(_shown_on(row)),  # noqa: F821
                _status(row, now),
                shell.time_html(row.get("checked_at"), now)
                if int(row.get("checked_at") or 0) else
                '<span class="muted">never</span>',
                shell.time_html(row.get("created_at"), now),
                toggle,
                _action_form("delete_instance", row, "Delete", csrf_field,
                             admin_query, danger=True)))
    table = shell.table(
        "<th>Instance</th><th>Shown on</th><th>Status</th>"
        "<th>Last checked</th><th>Started</th>"
        '<th class="r">Actions</th>',
        body, "No instance has asked to be listed or to have a hexagon yet.")
    return shell.page_head(
        "Instances",
        "Instances started from /start, or self-hosted ones that joined from "
        "their own admin console, that asked for a homepage listing or a "
        "World hexagon.") + shell.card(
        title="Instance directory",
        description="Every 15 minutes up to %d instances are asked for "
        "/readyz. After %d missed checks in a row an instance is not shown, "
        "and after a day offline the admins get one ping asking whether to "
        "delete it. Hide keeps an instance off both even while it answers. "
        "An instance that joined by itself comes back after a Delete on its "
        "next announcement; Hide keeps it off."
        % (instance_directory.HEALTH_BATCH,
           instance_directory.OFFLINE_FAILURES),
        table=table)


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    runtime = EnvRuntime(env)
    if method_name(request) == "POST":  # noqa: F821
        return await _handle_post(env, request, runtime, admin_query, account)
    shell = _admin_shell  # noqa: F821
    notice = NOTICES.get(_first(params, "notice"))
    banner = shell.banner(notice[0], notice[1]) if notice else ""
    rows = await instance_directory.admin_rows(runtime)
    url = urlparse(request.url)
    return _html(shell.render_page(
        "Instances", banner + render(rows, csrf_field, admin_query,
                                     runtime.now()),
        active=VIEW, console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        subpages=shell.subpage_base(env),
        next_path=url.path + (("?" + url.query) if url.query else "")))


async def _handle_post(env, request, runtime, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = clean_hidden_query(parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True))

    def location(notice):
        return _console_path(env) + _admin_href(  # noqa: F821
            admin_query, view=VIEW, notice=notice).replace("&amp;", "&")

    if not _admin_csrf_ok(env, form):  # noqa: F821
        return _redirect(location("csrf"))
    action = _first(form, "action")
    entry_id = _first(form, "id")
    if action not in ACTIONS:
        return _redirect(location("invalid"))
    if action == "delete_instance":
        if _first(form, "confirm") != "yes":
            return _redirect(location("unconfirmed"))
        domain = await instance_directory.delete(runtime, entry_id)
        notice = "deleted"
    else:
        domain = await instance_directory.set_hidden(
            runtime, entry_id, action == "hide_instance")
        notice = "hidden" if action == "hide_instance" else "shown"
    if not domain:
        return _redirect(location("missing"))
    await _audit_sensitive_action(  # noqa: F821
        env, account, "admin.console_" + action, "instance_directory",
        domain, "success", {"id": entry_id})
    return _redirect(location(notice))
