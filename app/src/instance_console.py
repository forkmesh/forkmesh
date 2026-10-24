import json
import secrets
from urllib.parse import parse_qs, quote, urlparse
from hidden_unicode import clean_query as clean_hidden_query

import instance_features
import instance_home
import instance_registry
import instance_world


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


PLUGINS_VIEW = "plugins"
DIRECTORY_VIEW = "federation"
WORLD_VIEW = "world-territory"
HOME_VIEW = "home-page"
VIEWS = (PLUGINS_VIEW, DIRECTORY_VIEW, WORLD_VIEW, HOME_VIEW)
MAX_FORM_BYTES = 32 * 1024
POST_TIMEOUT_SECONDS = 20
NOTICES = {
    "directory_listed": ("Saved and announced: forkmesh.com lists this "
                         "instance.", "ok"),
    "directory_removed": ("Saved and announced: forkmesh.com no longer "
                          "lists this instance.", "ok"),
    "directory_saved": ("Saved. This instance is not in the directory.",
                        "ok"),
    "directory_failed": ("Saved, but forkmesh.com refused or did not answer. "
                         "The reason is below; the instance tries again "
                         "every hour.", "warn"),
    "directory_core": ("forkmesh.com is the directory itself: see "
                       "Instances.", "bad"),
    "plugins_saved": ("Plugins saved. Pages and menus follow within a "
                      "minute.", "ok"),
    "plugins_core": ("Plugins are chosen on self-hosted instances only.",
                     "bad"),
    "world_saved": ("World saved. Visitors see it on their next visit.",
                    "ok"),
    "world_invalid": ("Not saved: the World settings did not pass the "
                      "checks listed below.", "bad"),
    "world_core": ("The ForkMesh World is configured by forkmesh.com.",
                   "bad"),
    "home_saved": ("Home page saved. Visitors see it within a minute.", "ok"),
    "home_invalid": ("Not saved: name a repository as owner/name.", "bad"),
    "home_core": ("forkmesh.com keeps its own home page.", "bad"),
    "csrf": ("Action blocked: reload the page and try again.", "bad"),
    "invalid": ("Action failed: unknown action.", "bad"),
}
ACTIONS = {"save_plugins": PLUGINS_VIEW, "save_directory": DIRECTORY_VIEW,
           "announce_directory": DIRECTORY_VIEW, "save_world": WORLD_VIEW,
           "save_home": HOME_VIEW}
STATE_PILLS = {
    "listed": ("Listed", "ok"),
    "removed": ("Not listed", ""),
    "failed": ("Last announcement failed", "bad"),
    "": ("Not announced yet", "info"),
}


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


class AnnounceRuntime:
    def __init__(self, env):
        self.env = env

    def now(self):
        return _now()

    async def setting_doc(self):
        try:
            row = await d1_first(  # noqa: F821
                self.env, "SELECT doc FROM instance_settings WHERE k=?",
                instance_registry.SETTING_KEY)
        except Exception:
            return None
        return None if row is None else str(row.get("doc") or "")

    async def save_setting_doc(self, doc):
        await save_setting(self.env, instance_registry.SETTING_KEY, doc)

    def domain(self):
        return instance_registry.domain_of(
            getattr(self.env, "PUBLIC_BASE_URL", ""))

    def directory_url(self):
        return instance_registry.directory_url(
            getattr(self.env, "INSTANCE_DIRECTORY_URL", ""))

    def public_key(self):
        return _https_mirror_router_public_key(self.env)  # noqa: F821

    def nonce(self):
        return secrets.token_hex(16)

    async def sign(self, message):
        return str(await ed25519_sign(  # noqa: F821
            self.public_key(), _https_mirror_router_seed(self.env),  # noqa: F821
            message) or "")

    async def post_json(self, url, body, limit):
        try:
            response = await js_fetch_with_timeout(  # noqa: F821
                url, {"method": "POST", "redirect": "manual",
                      "headers": {"content-type": "application/json",
                                  "accept": "application/json"},
                      "body": json.dumps(body)}, POST_TIMEOUT_SECONDS)
        except Exception:
            return 0, {}
        status = int(getattr(response, "status", 0) or 0)
        try:
            text = str(await response.text())[:limit]
            reply = json.loads(text) if text else {}
        except Exception:
            reply = {}
        return status, reply if isinstance(reply, dict) else {}


async def announce_cron(env):
    if not instance_branded(env):  # noqa: F821
        return {"announced": False, "reason": "directory_host"}
    return await instance_registry.announce_if_due(AnnounceRuntime(env))


def _directory_status(setting, now):
    shell = _admin_shell  # noqa: F821
    status = setting["status"]
    label, tone = STATE_PILLS[status["state"]]
    rows = [("Status", shell.pill(label, tone))]
    if status["at"]:
        rows.append(("Last announced", shell.time_html(status["at"], now)))
    if status["state"] == "listed":
        places = []
        if status["sent"]["listed"]:
            places.append("the forkmesh.com homepage")
        if status["sent"]["world"]:
            places.append("World hexagon %d" % (status["slot"] + 1)
                          if status["slot"] >= 0 else
                          "the World, waiting for a free hexagon")
        rows.append(("Shown on", _esc(", ".join(places))))
    if status["message"]:
        rows.append(("Message", _esc(status["message"])))
    return shell.table("<th>Directory</th><th></th>", [
        "<tr><td>%s</td><td>%s</td></tr>" % (_esc(key), value)
        for key, value in rows])


async def render_directory(env, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    head = shell.page_head(
        "ForkMesh directory",
        "List this instance on the forkmesh.com homepage and give it a "
        "hexagon in the ForkMesh World.", crumb="Platform")
    if not instance_branded(env):  # noqa: F821
        return head + shell.card(
            title="This server is the directory",
            description="Self-hosted instances announce themselves here. "
            "Platform -> Instances lists them.")
    runtime = AnnounceRuntime(env)
    doc = await runtime.setting_doc()
    setting = instance_registry.parse_setting(doc or "")
    world_on = "world" in instance_features.enabled_ids(env)
    domain = runtime.domain()
    choices = (
        '<form method="post" action="%s" class="stack">%s'
        '<input type="hidden" name="action" value="save_directory">'
        '%s<p class="hint">Lists the name, logo and address of this '
        'instance on forkmesh.com.</p>'
        '%s<p class="hint">%s</p>'
        '<div><button type="submit" class="btn btn-primary">Save and '
        'announce</button></div></form>' % (
            _admin_href(admin_query, view=DIRECTORY_VIEW),  # noqa: F821
            csrf_field,
            shell.switch("listed", "List on the forkmesh.com homepage",
                         setting["listed"], "1", "directory-listed"),
            shell.switch("world", "Give it a hexagon in the ForkMesh World",
                         setting["world"], "1", "directory-world"),
            "Its hexagon is a portal into this instance's own World, and "
            "this World gets a portal back." if world_on else
            "The World plugin is off here, so the hexagon opens this "
            "instance's home page. Turn the World plugin on under Plugins "
            "for a portal into its own World."))
    announce = (
        '<form method="post" action="%s">%s'
        '<input type="hidden" name="action" value="announce_directory">'
        '<button type="submit" class="btn">Announce now</button></form>' % (
            _admin_href(admin_query, view=DIRECTORY_VIEW),  # noqa: F821
            csrf_field)) if doc is not None else ""
    how = (
        "Saving publishes %s and asks %s to read it. forkmesh.com checks "
        "that the document is served by %s over HTTPS and that the request "
        "is signed by the key it names, then lists the instance and checks "
        "/readyz every 15 minutes. The instance announces again every six "
        "hours, so a new name or logo follows. Turning both off removes "
        "it." % (instance_registry.DOC_PATH,
                 runtime.directory_url() or "forkmesh.com",
                 domain or "this domain"))
    return head + shell.card(
        title="Listing", description=how, body=choices,
        actions=announce) + shell.card(
        title="Last announcement", table=_directory_status(setting, _now()))


async def save_directory(env, form):
    if not instance_branded(env):  # noqa: F821
        return "directory_core", None
    runtime = AnnounceRuntime(env)
    setting = instance_registry.parse_setting(
        await runtime.setting_doc() or "")
    setting["listed"] = _first(form, "listed") == "1"
    setting["world"] = _first(form, "world") == "1"
    await runtime.save_setting_doc(instance_registry.dump_setting(setting))
    return await _announce(runtime, setting)


async def announce_directory(env):
    if not instance_branded(env):  # noqa: F821
        return "directory_core", None
    runtime = AnnounceRuntime(env)
    doc = await runtime.setting_doc()
    if doc is None:
        return "directory_saved", {"announced": False}
    return await _announce(runtime, instance_registry.parse_setting(doc))


async def _announce(runtime, setting):
    details = instance_registry.choice(setting)
    if not instance_registry.participating(setting) and \
            setting["status"]["state"] in ("", "removed"):
        return "directory_saved", dict(details, announced=False)
    result = await instance_registry.announce(runtime, setting)
    state = result.get("state") or "failed"
    notice = {"listed": "directory_listed",
              "removed": "directory_removed"}.get(state, "directory_failed")
    return notice, dict(details, announced=True, state=state)


WORLD_EXAMPLE = {
    "style": "security",
    "accent": "#e74c3c",
    "objects": [{"type": "house", "label": "Hacker House",
                 "url": "https://hackerhouse.example.org/",
                 "position": [-60, -10]}],
    "bugs": {"repo": "acme/website", "ref": "main",
             "path": ".forkmesh/world/bugs.json", "title": "Bug terrarium",
             "limit": 60, "position": [60, -10]},
}


async def render_world(env, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    head = shell.page_head(
        "World",
        "This instance's own hexagon in the World: its style, buildings "
        "that link to a website, and the bug terrarium.", crumb="Platform")
    if not instance_branded(env):  # noqa: F821
        return head + shell.card(
            title="This server is the ForkMesh World",
            description="Self-hosted instances configure their own "
            "hexagon here.")
    setting = instance_world.parse(
        await settings_doc(env, instance_world.SETTING_KEY))
    form = (
        '<form method="post" action="%s" class="stack">%s'
        '<input type="hidden" name="action" value="save_world">'
        '<textarea name="world" rows="22" spellcheck="false" '
        'style="width:100%%;font-family:ui-monospace,monospace;'
        'font-size:12px">%s</textarea>'
        '<div><button type="submit" class="btn btn-primary">Save World'
        '</button></div></form>' % (
            _admin_href(admin_query, view=WORLD_VIEW),  # noqa: F821
            csrf_field, _esc(instance_world.pretty(setting))))
    rules = (
        "<p>style: %s. accent: a #rrggbb colour (empty uses the instance "
        "theme). objects: up to %d buildings, each {type: \"house\", label, "
        "url (https), position [x, z] from the plaza, within %d and inside "
        "the hexagon}; clicking one leaves the World for its url. bugs: the "
        "terrarium reads a JSON "
        "list of {domain, name, url, favicon} from a file in one of this "
        "instance's repositories (repo owner/name, ref, path); favicon is a "
        "png, ico, gif, jpg or webp path next to that file; at most %d "
        "bugs are shown. Set bugs to null to remove the terrarium.</p>"
        "<p>Example:</p><pre style=\"font-size:12px\">%s</pre>" % (
            _esc(", ".join(instance_world.STYLES)),
            instance_world.MAX_OBJECTS, instance_world.REACH,
            instance_world.MAX_BUGS,
            _esc(instance_world.pretty(WORLD_EXAMPLE))))
    return head + shell.card(
        title="World settings", description="Saved as JSON.",
        body=form) + shell.card(title="What each setting does", body=rules)


async def save_world(env, form):
    if not instance_branded(env):  # noqa: F821
        return "world_core", None
    try:
        setting = instance_world.from_text(_first(form, "world"))
        doc = instance_world.dump(setting)
    except instance_world.SettingError as error:
        return "world_invalid", {"error": str(error)[:200]}
    await save_setting(env, instance_world.SETTING_KEY, doc)
    return "world_saved", {
        "style": setting["style"], "objects": len(setting["objects"]),
        "bugs": bool(setting["bugs"])}


async def render_home(env, csrf_field, admin_query):
    shell = _admin_shell  # noqa: F821
    head = shell.page_head(
        "Home page",
        "Choose a repository whose README, latest threads and labels make up "
        "this instance's front page.", crumb="Platform")
    if not instance_branded(env):  # noqa: F821
        return head + shell.card(
            title="forkmesh.com keeps its own home page",
            description="Self-hosted instances choose a home repository here.")
    current = instance_home.parse(await settings_doc(env, instance_home.SETTING_KEY))
    form = (
        '<form method="post" action="%s" class="stack">%s'
        '<input type="hidden" name="action" value="save_home">'
        '<label>Home repository <input name="repository" value="%s" '
        'placeholder="owner/name" autocomplete="off" spellcheck="false"></label>'
        '<div><button type="submit" class="btn btn-primary">Save home page'
        '</button></div></form>' % (
            _admin_href(admin_query, view=HOME_VIEW),  # noqa: F821
            csrf_field, _esc(current)))
    about = (
        "<p>With a home repository, / shows its README (each section keeps its "
        "own reading direction), the latest threads from its issues, its labels "
        "and links to the Atom feeds and the lite view. Without one, / opens the "
        "dashboard. The repository must be public. Leave the field empty to "
        "turn the page off. The page, the feeds and the lite view are served "
        "only while the Feeds and lite view plugin is on (Plugins).</p>")
    return head + shell.card(
        title="Home repository",
        description="Now: %s" % (_esc(current) if current else "none, / opens the dashboard"),
        body=form) + shell.card(title="What visitors see", body=about)


async def save_home(env, form):
    if not instance_branded(env):  # noqa: F821
        return "home_core", None
    try:
        repository = instance_home.clean(_first(form, "repository"))
    except instance_home.SettingError:
        return "home_invalid", None
    await save_setting(env, instance_home.SETTING_KEY, instance_home.dump(repository))
    return "home_saved", {"repository": repository}


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    view = _first(params, "view")
    if method_name(request) == "POST":  # noqa: F821
        return await _handle_post(env, request, view, admin_query, account)
    shell = _admin_shell  # noqa: F821
    notice = NOTICES.get(_first(params, "notice"))
    banner = shell.banner(notice[0], notice[1]) if notice else ""
    if view == DIRECTORY_VIEW:
        title = "ForkMesh directory"
        content = await render_directory(env, csrf_field, admin_query)
    elif view == WORLD_VIEW:
        title = "World"
        content = await render_world(env, csrf_field, admin_query)
    elif view == HOME_VIEW:
        title = "Home page"
        content = await render_home(env, csrf_field, admin_query)
    else:
        title = "Plugins"
        content = await render_plugins(env, csrf_field, admin_query)
    url = urlparse(request.url)
    return _html(shell.render_page(
        title, banner + content, active=view,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        next_path=url.path + (("?" + url.query) if url.query else ""),
        subpages=shell.subpage_base(env)))


async def _handle_post(env, request, view, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = clean_hidden_query(parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True))
    action = _first(form, "action")
    target = ACTIONS.get(action) or (view if view in VIEWS else PLUGINS_VIEW)

    def location(notice):
        return _console_path(env) + _admin_href(  # noqa: F821
            admin_query, view=target, notice=notice).replace("&amp;", "&")

    if not _admin_csrf_ok(env, form):  # noqa: F821
        return _redirect(location("csrf"))
    if action not in ACTIONS:
        return _redirect(location("invalid"))
    if action == "save_plugins":
        notice, details = await save_plugins(env, form)
    elif action == "save_directory":
        notice, details = await save_directory(env, form)
    elif action == "save_world":
        notice, details = await save_world(env, form)
    elif action == "save_home":
        notice, details = await save_home(env, form)
    else:
        notice, details = await announce_directory(env)
    await _audit_sensitive_action(  # noqa: F821
        env, account, "admin.console_" + action, "instance_settings",
        target, "success" if NOTICES[notice][1] == "ok" else "refused",
        dict(details or {}, notice=notice))
    return _redirect(location(notice))
