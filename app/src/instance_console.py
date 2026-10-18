from urllib.parse import parse_qs, quote, urlparse

import instance_features


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PLUGINS_VIEW = "plugins"
VIEWS = (PLUGINS_VIEW,)
MAX_FORM_BYTES = 8 * 1024
NOTICES = {
    "plugins_saved": ("Plugins saved. Pages and menus follow within a "
                      "minute.", "ok"),
    "plugins_core": ("Plugins are chosen on self-hosted instances only.",
                     "bad"),
    "csrf": ("Action blocked: reload the page and try again.", "bad"),
    "invalid": ("Action failed: unknown action.", "bad"),
}
ACTIONS = {"save_plugins": PLUGINS_VIEW}


def _first(params, key):
    return str((params.get(key) or [""])[0] or "")


def _now():
    return int(Date.now())  # noqa: F821


def _esc(value):
    return _html_escape(value)  # noqa: F821


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + quote(path, safe="/-_.~") if path else ""


def _html(body):
    return Response(body, status=200, headers={  # noqa: F821
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


def _redirect(location):
    return Response("", status=303, headers={  # noqa: F821
        "location": location, "cache-control": "no-store"})


async def settings_doc(env, key):
    try:
        row = await d1_first(  # noqa: F821
            env, "SELECT doc FROM instance_settings WHERE k=?", key)
    except Exception:
        row = None
    return (row or {}).get("doc") or ""


async def save_setting(env, key, doc):
    await d1_run(  # noqa: F821
        env,
        "INSERT INTO instance_settings (k, doc, updated_at) VALUES (?,?,?) "
        "ON CONFLICT(k) DO UPDATE SET doc=excluded.doc, "
        "updated_at=excluded.updated_at", key, doc, _now())


async def enabled_plugins(env):
    stored = instance_features.parse(
        await settings_doc(env, instance_features.SETTING_KEY))
    return stored if stored is not None else instance_features.installed_ids(
        env)


def _plugin_row(feature, on, editable=True):
    plugin_id, title = feature[0], feature[1]
    note = (instance_features.FORKMESH_ONLY.get(plugin_id)
            or instance_features.INSTANCE_NOTES.get(plugin_id, ""))
    return (
        '<tr><td>%s</td><td><strong>%s</strong><br>'
        '<span class="muted">%s</span></td><td class="muted">%s</td></tr>' % (
            _admin_shell.switch(  # noqa: F821
                "plugin", "", on, plugin_id, "plugin-" + plugin_id)
            if editable else _admin_shell.pill(  # noqa: F821
                "On" if on else "Off", "ok" if on else ""),
            _esc(title), _esc(feature[2]), _esc(note)))


async def render_plugins(env, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    head = shell.page_head(
        "Plugins",
        "Choose the optional parts of this instance. A plugin that is off "
        "answers not found and its menus are hidden.", crumb="Platform")
    if not instance_branded(env):  # noqa: F821
        return head + shell.card(
            title="Every plugin is on",
            description="This server is forkmesh.com, where every plugin "
            "runs. Plugins are chosen on self-hosted instances.")
    on = await enabled_plugins(env)
    stored = instance_features.parse(
        await settings_doc(env, instance_features.SETTING_KEY)) is not None
    choosable = [feature for feature in instance_features.FEATURES
                 if feature[0] in instance_features.CHOOSABLE]
    forkmesh = [feature for feature in instance_features.FEATURES
                if feature[0] in instance_features.FORKMESH_ONLY]
    rows = [_plugin_row(feature, feature[0] in on) for feature in choosable]
    source = (
        "Saved from this page." if stored else
        "Set by the install settings (INSTANCE_PLUGINS) until you save here; "
        "after that this page decides and updates keep your choice.")
    form = (
        '<form method="post" action="%s">%s'
        '<input type="hidden" name="action" value="save_plugins">%s'
        '<div class="card-b"><button type="submit" class="btn btn-primary">'
        'Save plugins</button></div></form>' % (
            _admin_href(admin_query, view=PLUGINS_VIEW),  # noqa: F821
            csrf_field, shell.table(
                "<th>On</th><th>Plugin</th><th>On this instance</th>", rows)))
    fixed = shell.table(
        "<th>State</th><th>Plugin</th><th>Why it is not offered</th>",
        [_plugin_row(feature, feature[0] in on, False)
         for feature in forkmesh])
    return head + shell.card(
        title="Plugins for this instance", description=source,
        table=form) + shell.card(
        title="forkmesh.com services",
        description="These plugins need forkmesh.com's own accounts, "
        "ledger or content, so they are not offered here. An operator can "
        "still list one in INSTANCE_PLUGINS; turning it off happens there "
        "too.", table=fixed)


async def save_plugins(env, form):
    if not instance_branded(env):  # noqa: F821
        return "plugins_core", None
    before = await enabled_plugins(env)
    chosen = {item for item in form.get("plugin") or [] if item}
    after = instance_features.choose(before, chosen)
    await save_setting(env, instance_features.SETTING_KEY,
                       instance_features.dump(after))
    instance_features.remember(after, _now())
    return "plugins_saved", {
        "enabled": sorted(after),
        "turnedOn": sorted(after - before),
        "turnedOff": sorted(before - after),
    }


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    view = _first(params, "view")
    if method_name(request) == "POST":  # noqa: F821
        return await _handle_post(env, request, view, admin_query, account)
    shell = _admin_shell  # noqa: F821
    notice = NOTICES.get(_first(params, "notice"))
    banner = shell.banner(notice[0], notice[1]) if notice else ""
    content = await render_plugins(env, csrf_field, admin_query)
    url = urlparse(request.url)
    return _html(shell.render_page(
        "Plugins", banner + content, active=view,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        next_path=url.path + (("?" + url.query) if url.query else "")))


async def _handle_post(env, request, view, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True)
    action = _first(form, "action")
    target = ACTIONS.get(action) or (view if view in VIEWS else PLUGINS_VIEW)

    def location(notice):
        return _console_path(env) + _admin_href(  # noqa: F821
            admin_query, view=target, notice=notice).replace("&amp;", "&")

    if not _admin_csrf_ok(env, form):  # noqa: F821
        return _redirect(location("csrf"))
    if action not in ACTIONS:
        return _redirect(location("invalid"))
    notice, details = await save_plugins(env, form)
    await _audit_sensitive_action(  # noqa: F821
        env, account, "admin.console_" + action, "instance_settings",
        target, "success" if NOTICES[notice][1] == "ok" else "refused",
        dict(details or {}, notice=notice))
    return _redirect(location(notice))
