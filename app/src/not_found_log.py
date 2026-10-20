import ipaddress
import re
from urllib.parse import quote


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "not-found"
ALERT_HITS = 10
ALERTS_PER_HOUR = 5
MAX_GROUPS = 5000
RETAIN_MS = 90 * 24 * 60 * 60 * 1000
PRUNE_EVERY_NEW_GROUPS = 50
PATH_MAX = 300
SITE_MAX = 120
REFERRER_MAX = 200
USER_AGENT_MAX = 300
IP_MAX = 64
PAGE_SIZE = 50
MAX_FORM_BYTES = 4096
HOUR_MS = 60 * 60 * 1000
DAY_MS = 24 * HOUR_MS
CHART_HOURS = 24
HOURS_RETAINED = 48
MAX_IGNORES = 500
IGNORE_CACHE_MS = 60 * 1000
METHODS = ("GET", "POST", "PUT", "PATCH", "DELETE", "OPTIONS")
SORTS = {"hits": "Most hits", "today": "Busiest today",
         "recent": "Most recent"}
SORT_SQL = {
    "hits": "p.hits DESC, p.last_seen DESC",
    "today": "day_hits DESC, p.last_seen DESC",
    "recent": "p.last_seen DESC, p.hits DESC",
}
NOTICES = {
    "cleared": ("Group cleared. It starts counting again from its next hit.",
                "ok"),
    "missing": ("That group was already gone.", "ok"),
    "invalid": ("Clear failed: invalid group.", "bad"),
    "ignored": ("Exception added. 404s on that path are no longer logged.",
                "ok"),
    "unignored": ("Exception removed. 404s on that path are logged again.",
                  "ok"),
    "rule_missing": ("That exception was already gone.", "ok"),
    "rule_invalid": ("Exception not added: pick a site and a path. A whole "
                     "site cannot be ignored.", "bad"),
    "rule_full": ("Exception not added: the list already holds %d entries."
                  % MAX_IGNORES, "bad"),
    "csrf": ("Action blocked: invalid or missing CSRF token. Reload the page "
             "and try again.", "bad"),
    "ban_lifted": ("Ban lifted. The address can reach every site again within "
                   "a minute.", "ok"),
    "ban_missing": ("That ban was already gone.", "ok"),
    "ban_invalid": ("Lift failed: invalid ban.", "bad"),
}
_SEGMENT_ID_RE = re.compile(
    r"^(?:[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{12}|[0-9a-fA-F]{16,}|(?=[A-Za-z0-9_-]*[0-9])"
    r"[A-Za-z0-9_-]{24,})$")
_SITE_RE = re.compile(r"^[a-z0-9.-]+(?::[0-9]{1,5})?$")
_CONTROL_RE = re.compile(r"[\x00-\x20\x7f]")
_GIT_RE = re.compile(r"(?:\.git(?:/|$)|/info/refs$|/git-(?:upload|receive)-pack$)")
_COUNTRY_RE = re.compile(r"^[A-Z]{2}$")
_NO_COUNTRY = ("XX", "T1")
_PRUNE_STATE = {"new_groups": 0, "hour": -1}
_IGNORE_CACHE = {"at": 0, "rules": None}


def _now():
    return int(Date.now())


def _first(params, key):
    values = (params or {}).get(key) or [""]
    return str(values[0] or "")


def normalize_path(path):
    path = _CONTROL_RE.sub("", str(path or ""))
    if not path.startswith("/"):
        return ""
    segments = [
        "[id]" if _SEGMENT_ID_RE.match(segment) else segment
        for segment in path.split("/")
    ]
    return "/".join(segments)[:PATH_MAX]


def normalize_site(host):
    host = str(host or "").strip().lower().rstrip(".")
    if host.startswith("www."):
        host = host[4:]
    host = host[:SITE_MAX]
    return host if _SITE_RE.match(host) else ""


def normalize_method(method):
    method = str(method or "").upper()
    if method == "HEAD":
        return "GET"
    return method if method in METHODS else "OTHER"


def _origin_host(value):
    try:
        return normalize_site(urlparse(str(value or "")).hostname or "")
    except Exception:
        return ""


def _own_sites(env):
    sites = set()
    for name in ("APP_ORIGIN", "WWW_ORIGIN", "WORLD_ORIGIN"):
        site = _origin_host(getattr(env, name, "") or "")
        if site:
            sites.add(site)
    return sites


def referrer_label(env, value):
    try:
        parsed = urlparse(str(value or "").strip())
    except Exception:
        return ""
    if parsed.scheme not in ("http", "https"):
        return ""
    site = normalize_site(parsed.hostname or "")
    if not site:
        return ""
    if site in _own_sites(env):
        return (site + normalize_path(parsed.path or "/"))[:REFERRER_MAX]
    return site


def _header(request, name):
    try:
        headers = getattr(request, "headers", None)
        return str((headers.get(name) if headers is not None else "") or "")
    except Exception:
        return ""


def normalize_ip(value):
    try:
        return str(ipaddress.ip_address(
            str(value or "").strip().strip("[]")[:IP_MAX]))
    except ValueError:
        return ""


def normalize_country(value):
    country = str(value or "").strip().upper()
    return country if _COUNTRY_RE.match(country) and (
        country not in _NO_COUNTRY) else ""


def normalize_user_agent(value):
    return _CONTROL_RE.sub(" ", str(value or "")).strip()[:USER_AGENT_MAX]


def request_client(request, user_agent=None):
    return {
        "ip": normalize_ip(_header(request, "cf-connecting-ip")),
        "country": normalize_country(_header(request, "cf-ipcountry")),
        "ua": normalize_user_agent(
            _header(request, "user-agent") if user_agent is None
            else user_agent),
    }


def country_flag(country):
    country = normalize_country(country)
    return "".join(chr(0x1F1E6 + ord(c) - 65) for c in country)


async def _seal_client(env, client):
    client = {key: value for key, value in (client or {}).items() if value}
    if not client:
        return ""
    try:
        return await encrypt_row(env, client)
    except Exception:
        return ""


async def _open_client(env, sealed):
    if not sealed:
        return {}
    try:
        client = await decrypt_row(env, sealed)
    except Exception:
        return {}
    return client if isinstance(client, dict) else {}


def source_label(path):
    path = str(path or "")
    if path == "/api" or path.startswith("/api/"):
        return "API"
    if _GIT_RE.search(path):
        return "Git"
    last = path.rsplit("/", 1)[-1]
    if "." in last.strip("."):
        return "File"
    return "Page"


def _forget_ignores():
    _IGNORE_CACHE["rules"] = None


async def _ignore_rules(env, now):
    rules = _IGNORE_CACHE["rules"]
    if rules is not None and 0 <= now - _IGNORE_CACHE["at"] < IGNORE_CACHE_MS:
        return rules
    rows = await d1_all(
        env, "SELECT site,path,prefix FROM not_found_ignores LIMIT ?",
        MAX_IGNORES)
    rules = [
        (str(row.get("site") or ""), str(row.get("path") or ""),
         bool(int(row.get("prefix") or 0)))
        for row in (dict(item) for item in rows or [])]
    _IGNORE_CACHE.update(at=now, rules=rules)
    return rules


def ignore_match(rules, site, path):
    for rule_site, rule_path, prefix in rules or []:
        if rule_site != site:
            continue
        if path == rule_path or (prefix and path.startswith(rule_path + "/")):
            return rule_path
    return ""


def _hour_of(ms):
    return int(ms) // HOUR_MS


def _under_sql(prefix):
    return ("(path=? OR substr(path,1,?)=?)" if prefix else "path=?")


def _under_args(path, prefix):
    return (path, len(path) + 1, path + "/") if prefix else (path,)


async def record(env, site, method, path, referrer="", now=None,
                 client=None, fetch_site=""):
    try:
        site = normalize_site(site)
        method = normalize_method(method)
        path = normalize_path(path)
        if not site or not path:
            return False
        now = int(now or _now())
        await ensure_schema(env)
        await _probe_ban.observe(env, path, client, now, fetch_site)
        matched = ignore_match(await _ignore_rules(env, now), site, path)
        if matched:
            await d1_run(
                env,
                "UPDATE not_found_ignores SET hits=hits+1,last_seen=? "
                "WHERE site=? AND path=?",
                now, site, matched)
            return False
        sealed = await _seal_client(env, client)
        rows = await d1_all(
            env,
            "INSERT INTO not_found_paths "
            "(site,method,path,hits,first_seen,last_seen,referrer,last_client) "
            "VALUES (?,?,?,1,?,?,?,?) "
            "ON CONFLICT(site,method,path) DO UPDATE SET "
            "hits=hits+1,last_seen=excluded.last_seen,"
            "referrer=CASE WHEN excluded.referrer<>'' "
            "THEN excluded.referrer ELSE referrer END,"
            "last_client=excluded.last_client "
            "RETURNING hits,first_seen,alerted_at,referrer",
            site, method, path, now, now, str(referrer or "")[:REFERRER_MAX],
            sealed)
        row = dict(rows[0]) if rows else {}
        hits = int(row.get("hits") or 0)
        await _count_hour(env, site, method, path, now)
        if hits == 1:
            await _maybe_prune(env, now)
        if hits >= ALERT_HITS and not int(row.get("alerted_at") or 0):
            await _maybe_alert(env, site, method, path, row, now)
        return True
    except Exception:
        return False


async def _count_hour(env, site, method, path, now):
    hour = _hour_of(now)
    await d1_run(
        env,
        "INSERT INTO not_found_hours (site,method,path,hour,hits) "
        "VALUES (?,?,?,?,1) ON CONFLICT(site,method,path,hour) "
        "DO UPDATE SET hits=hits+1",
        site, method, path, hour)
    if _PRUNE_STATE["hour"] == hour:
        return
    _PRUNE_STATE["hour"] = hour
    await d1_run(
        env, "DELETE FROM not_found_hours WHERE hour<=?",
        hour - HOURS_RETAINED)


async def _maybe_prune(env, now):
    count = _PRUNE_STATE["new_groups"]
    _PRUNE_STATE["new_groups"] = (count + 1) % PRUNE_EVERY_NEW_GROUPS
    if count:
        return
    await d1_run(
        env, "DELETE FROM not_found_paths WHERE last_seen<?", now - RETAIN_MS)
    await d1_run(
        env,
        "DELETE FROM not_found_paths WHERE rowid IN ("
        "SELECT rowid FROM not_found_paths "
        "ORDER BY last_seen DESC LIMIT -1 OFFSET ?)",
        MAX_GROUPS)


async def _maybe_alert(env, site, method, path, row, now):
    recent = await d1_first(
        env,
        "SELECT COUNT(*) AS n FROM not_found_paths "
        "WHERE alerted_at>0 AND alerted_at>=?",
        now - HOUR_MS)
    if int((recent or {}).get("n") or 0) >= ALERTS_PER_HOUR:
        return False
    claimed = await d1_all(
        env,
        "UPDATE not_found_paths SET alerted_at=? "
        "WHERE site=? AND method=? AND path=? AND alerted_at=0 "
        "RETURNING hits",
        now, site, method, path)
    if not claimed:
        return False
    hits = int(dict(claimed[0]).get("hits") or ALERT_HITS)
    await _alert_admins(env, site, method, path, hits,
                        str(row.get("referrer") or ""), now)
    return True


async def _alert_admins(env, site, method, path, hits, referrer, now):
    rows = await d1_all(
        env, "SELECT data FROM users WHERE is_admin=1 ORDER BY user_bi LIMIT 20")
    title = "404 reached %d hits: %s" % (hits, path[:80])
    body = "%s %s%s has answered 404 %d times." % (method, site, path, hits)
    if referrer:
        body += " Last linked from %s." % referrer
    delivered = 0
    for admin_row in rows or []:
        try:
            record = await decrypt_row(env, admin_row.get("data", "")) or {}
        except Exception:
            continue
        if record.get("service_account") is True:
            continue
        name = clean_string(record.get("name", ""), MAX_NODE_NAME).strip().lower()
        if not valid_node_name(name):
            continue
        try:
            if await enqueue_notification(
                    env, name, "operational_alert", title, body=body,
                    href=(_console_path(env) or "/admin") + "?view=" + VIEW,
                    source="not-found",
                    dedupe="not-found:%s:%s:%s" % (site, method, path[:160]),
                    ts=now,
                    meta={"status": 404, "site": site, "method": method,
                          "path": path, "hits": hits}):
                delivered += 1
        except Exception:
            continue
    return delivered


async def record_response(env, request, url):
    try:
        path = str(getattr(url, "path", "") or "")
        privacy_filter = globals().get("_privacy_safe_log_path")
        if callable(privacy_filter):
            path = await privacy_filter(env, path)
        return await record(
            env, getattr(url, "hostname", "") or "", method_name(request),
            path, referrer_label(env, _header(request, "referer")),
            client=request_client(request),
            fetch_site=_header(request, "sec-fetch-site"))
    except Exception:
        return False


async def relay_handler(env, request):
    if method_name(request) != "POST":
        return json_response(
            {"error": "method_not_allowed"}, status=405,
            cache_control="no-store", extra_headers={"allow": "POST"})
    try:
        data = await bounded_json_request(request)
    except Exception:
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control="no-store")
    if not isinstance(data, dict):
        return json_response(
            {"error": "invalid_json"}, status=400, cache_control="no-store")
    path = normalize_path(data.get("path"))
    site = _origin_host(getattr(env, "WWW_ORIGIN", "") or "")
    if not path or not site:
        return json_response(
            {"error": "bad_request"}, status=400, cache_control="no-store")
    user_agent = data.get("userAgent")
    fetch_site = data.get("fetchSite")
    recorded = await record(
        env, site, "GET", path, referrer_label(env, data.get("referrer")),
        client=request_client(
            request, user_agent if isinstance(user_agent, str) else ""),
        fetch_site=fetch_site if isinstance(fetch_site, str) else "")
    return json_response(
        {"ok": True, "recorded": bool(recorded)}, cache_control="no-store")


def _html(body, status=200):
    return Response(body, status=status, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


def _redirect(location):
    return Response("", status=303, headers={
        "location": location, "cache-control": "no-store"})


def _console_path(env):
    path = str(getattr(env, "ADMIN_PATH", "") or "").strip("/")
    return "/" + quote(path, safe="/-_.~") if path else ""


def _page(params):
    raw = _first(params, "page")
    return max(1, int(raw)) if raw.isdigit() and len(raw) <= 6 else 1


def _sort(params):
    sort = _first(params, "sort")
    return sort if sort in SORTS else "hits"


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    await ensure_schema(env)
    if method_name(request) == "POST":
        return await _handle_post(env, request, params, admin_query, account)
    shell = _admin_shell
    banner = ""
    notice = NOTICES.get(_first(params, "notice"))
    if notice:
        banner = shell.banner(notice[0], notice[1])
    content = await render(env, params, csrf_field, admin_query)
    url = urlparse(request.url)
    page = shell.render_page(
        "404s", banner + content, active=VIEW,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        subpages=shell.subpage_base(env),
        next_path=url.path + (("?" + url.query) if url.query else ""))
    return _html(page)


async def _handle_post(env, request, params, admin_query, account):
    try:
        raw = await request.text()
    except Exception:
        raw = ""
    form = parse_qs(str(raw or "")[:MAX_FORM_BYTES], keep_blank_values=True)
    back = dict(view=VIEW, sort=_first(params, "sort") or None,
                page=_first(params, "page") or None)

    def location(notice):
        return _console_path(env) + _admin_href(
            admin_query, **back, notice=notice).replace("&amp;", "&")

    if not _admin_csrf_ok(env, form):
        return _redirect(location("csrf"))
    action = _first(form, "action")
    if action == "ignore_not_found":
        return _redirect(location(await _add_ignore(env, form, account)))
    if action == "unignore_not_found":
        return _redirect(location(await _remove_ignore(env, form, account)))
    if action == "lift_probe_ban":
        return _redirect(location(await _probe_ban.lift(
            env, _first(form, "rule_id"), account)))
    site = normalize_site(_first(form, "site"))
    method = _first(form, "method")
    path = _first(form, "path")[:PATH_MAX]
    if (action != "clear_not_found" or not site
            or method not in METHODS + ("OTHER",) or not path.startswith("/")):
        return _redirect(location("invalid"))
    existing = await d1_first(
        env,
        "SELECT hits FROM not_found_paths WHERE site=? AND method=? AND path=?",
        site, method, path)
    if not existing:
        return _redirect(location("missing"))
    await d1_run(
        env,
        "DELETE FROM not_found_paths WHERE site=? AND method=? AND path=?",
        site, method, path)
    await d1_run(
        env,
        "DELETE FROM not_found_hours WHERE site=? AND method=? AND path=?",
        site, method, path)
    await _audit_sensitive_action(
        env, account, "admin.console_clear_not_found", "not_found_paths",
        site + " " + method + " " + path, "success",
        {"hits": int(dict(existing).get("hits") or 0)})
    return _redirect(location("cleared"))


async def _add_ignore(env, form, account):
    site = normalize_site(_first(form, "site"))
    prefix = _first(form, "prefix") == "1"
    path = normalize_path(_first(form, "path").strip())
    if prefix:
        path = path.rstrip("/")
    if not site or not path:
        return "rule_invalid"
    known = await d1_first(
        env, "SELECT prefix FROM not_found_ignores WHERE site=? AND path=?",
        site, path)
    if not known:
        count = await d1_first(
            env, "SELECT COUNT(*) AS n FROM not_found_ignores")
        if int(dict(count or {}).get("n") or 0) >= MAX_IGNORES:
            return "rule_full"
    now = _now()
    await d1_run(
        env,
        "INSERT INTO not_found_ignores (site,path,prefix,created_at,created_by) "
        "VALUES (?,?,?,?,?) ON CONFLICT(site,path) DO UPDATE SET "
        "prefix=excluded.prefix",
        site, path, 1 if prefix else 0, now, str(account or "")[:80])
    removed = await d1_all(
        env,
        "DELETE FROM not_found_paths WHERE site=? AND " + _under_sql(prefix)
        + " RETURNING hits",
        site, *_under_args(path, prefix))
    await d1_run(
        env,
        "DELETE FROM not_found_hours WHERE site=? AND " + _under_sql(prefix),
        site, *_under_args(path, prefix))
    _forget_ignores()
    await _audit_sensitive_action(
        env, account, "admin.console_ignore_not_found", "not_found_ignores",
        site + " " + path + (" and below" if prefix else ""), "success",
        {"prefix": prefix, "groups": len(removed or []),
         "hits": sum(int(dict(row).get("hits") or 0) for row in removed or [])})
    return "ignored"


async def _remove_ignore(env, form, account):
    site = normalize_site(_first(form, "site"))
    path = _first(form, "path")[:PATH_MAX]
    if not site or not path.startswith("/"):
        return "rule_missing"
    removed = await d1_all(
        env,
        "DELETE FROM not_found_ignores WHERE site=? AND path=? "
        "RETURNING prefix,hits",
        site, path)
    if not removed:
        return "rule_missing"
    _forget_ignores()
    row = dict(removed[0])
    await _audit_sensitive_action(
        env, account, "admin.console_unignore_not_found", "not_found_ignores",
        site + " " + path + (" and below" if int(row.get("prefix") or 0)
                             else ""), "success",
        {"suppressed": int(row.get("hits") or 0)})
    return "unignored"


def _row_form(action, row, names, label, title, csrf_field, admin_query, sort,
              page, danger=False):
    fields = "".join(
        '<input type="hidden" name="%s" value="%s">' % (
            name, _html_escape(row.get(name) or ""))
        for name in names)
    return (
        '<form method="post" action="%s" style="display:inline">%s%s'
        '<input type="hidden" name="action" value="%s">'
        '<button type="submit" class="btn btn-sm%s" title="%s">%s</button>'
        '</form>'
        % (_admin_href(admin_query, view=VIEW, sort=sort,
                       page=page if page > 1 else None),
           csrf_field, fields, action, " btn-danger" if danger else "",
           title, label))


def _clear_form(row, csrf_field, admin_query, sort, page):
    return _row_form(
        "clear_not_found", row, ("site", "method", "path"), "Clear",
        "Forget this group so it counts and alerts again from its next hit",
        csrf_field, admin_query, sort, page)


def _ignore_form(row, csrf_field, admin_query, sort, page):
    return _row_form(
        "ignore_not_found", row, ("site", "path"), "Ignore",
        "Add an exception: stop logging 404s for this path, for something "
        "that was deleted on purpose",
        csrf_field, admin_query, sort, page, danger=True)


def _hour_series(text, first_hour):
    values = [0] * CHART_HOURS
    for item in str(text or "").split(","):
        hour, _sep, hits = item.partition(":")
        if not (hour.isdigit() and hits.isdigit()):
            continue
        index = int(hour) - first_hour
        if 0 <= index < CHART_HOURS:
            values[index] += int(hits)
    return values


def _hour_label(index, count):
    hours_ago = CHART_HOURS - 1 - index
    label = "current hour" if hours_ago == 0 else "%d hours ago" % hours_ago
    return "%s: %d hit%s" % (label, count, "" if count == 1 else "s")


async def _render_chart(env, first_hour):
    shell = _admin_shell
    rows = await d1_all(
        env,
        "SELECT hour, SUM(hits) AS hits FROM not_found_hours WHERE hour>=? "
        "GROUP BY hour",
        first_hour)
    hourly = _hour_series(
        ",".join("%d:%d" % (int(dict(row).get("hour") or 0),
                            int(dict(row).get("hits") or 0))
                 for row in rows or []),
        first_hour)
    total = sum(hourly)
    return shell.card(
        shell.hour_bars(
            hourly, ("24 hours ago", "18 h", "12 h", "6 h", "current hour"),
            _hour_label),
        title="Previous 24 hours",
        description="%s hit%s by hour, exceptions not counted" % (
            shell.format_int(total), "" if total == 1 else "s"),
        cls="not-found-chart")


async def _render_ignores(env, csrf_field, admin_query, sort, page, now):
    shell = _admin_shell
    rules = await d1_all(
        env,
        "SELECT site,path,prefix,created_at,created_by,hits,last_seen "
        "FROM not_found_ignores ORDER BY site, path LIMIT ?",
        MAX_IGNORES)
    known = await d1_all(
        env, "SELECT DISTINCT site FROM not_found_paths LIMIT 20")
    sites = sorted(_own_sites(env) | {
        str(dict(row).get("site") or "") for row in known or []} - {""})
    body = []
    for rule in rules or []:
        rule = dict(rule)
        body.append(
            '<tr><td class="mono"><span class="muted">%s</span>%s%s</td>'
            "<td>%s%s</td>"
            '<td class="r">%s</td><td>%s</td>'
            '<td class="r">%s</td></tr>' % (
                _html_escape(rule.get("site") or ""),
                _html_escape(rule.get("path") or ""),
                (" " + shell.pill("and below")) if int(
                    rule.get("prefix") or 0) else "",
                shell.time_html(rule.get("created_at"), now),
                (' <span class="muted">by %s</span>' % _html_escape(
                    rule.get("created_by"))) if rule.get("created_by") else "",
                shell.format_int(rule.get("hits") or 0),
                shell.time_html(rule.get("last_seen"), now)
                if int(rule.get("last_seen") or 0) else
                '<span class="muted">never</span>',
                _row_form(
                    "unignore_not_found", rule, ("site", "path"), "Remove",
                    "Log 404s on this path again", csrf_field, admin_query,
                    sort, page)))
    table = shell.table(
        "<th>Path</th><th>Added</th>"
        '<th class="r">Suppressed</th><th>Last suppressed</th>'
        '<th class="r">Actions</th>',
        body, "No exceptions. Every 404 is logged.")
    options = "".join(
        '<option value="%s">%s</option>' % (_html_escape(site),
                                            _html_escape(site))
        for site in sites)
    add = (
        '<form method="post" action="%s" class="form-row">%s'
        '<input type="hidden" name="action" value="ignore_not_found">'
        '<select name="site" aria-label="Site">%s</select>'
        '<input type="text" name="path" class="grow mono" required '
        'maxlength="%d" placeholder="/path/that/was/deleted" '
        'aria-label="Path">'
        '<label class="hint"><input type="checkbox" name="prefix" value="1"> '
        "and everything under it</label>"
        '<button type="submit" class="btn btn-sm">Add exception</button>'
        "</form>" % (
            _admin_href(admin_query, view=VIEW, sort=sort,
                        page=page if page > 1 else None),
            csrf_field, options, PATH_MAX))
    return shell.card(
        add, title="Exceptions",
        description="404s on these paths are not logged: use one for a page, "
        "file or repository that was deleted on purpose. Suppressed hits are "
        "still counted here.",
        table=table, cls="not-found-ignores")


def _client_cell(client):
    ip = str(client.get("ip") or "")
    country = normalize_country(client.get("country"))
    user_agent = str(client.get("ua") or "")
    if not (ip or country or user_agent):
        return '<td class="muted">—</td>'
    place = ""
    if country:
        place = '<span title="%s">%s %s</span> ' % (
            _html_escape(country), country_flag(country), _html_escape(country))
    shown = user_agent or "no user agent"
    if len(shown) > 80:
        shown = shown[:80] + "…"
    return (
        '<td>%s<span class="mono">%s</span><br>'
        '<span class="muted" title="%s">%s</span></td>' % (
            place, _html_escape(ip or "no IP"), _html_escape(user_agent),
            _html_escape(shown)))


def _request_cell(row):
    site = str(row.get("site") or "")
    path = str(row.get("path") or "")
    method = str(row.get("method") or "")
    text = '<span class="muted">%s</span>%s' % (
        _html_escape(site), _html_escape(path))
    if method == "GET" and "[" not in path:
        text = '<a href="%s" rel="noopener noreferrer" target="_blank">%s</a>' % (
            _html_escape("https://" + site + quote(path, safe="/-_.~%!$&'()*+,;=:@")),
            text)
    return '<td class="mono"><b>%s</b> %s</td>' % (_html_escape(method), text)


async def render(env, params, csrf_field="", admin_query="", now=None):
    shell = _admin_shell
    now = int(now or _now())
    sort = _sort(params)
    page = _page(params)
    summary = await d1_first(
        env,
        "SELECT COUNT(*) AS group_count, COALESCE(SUM(hits),0) AS hits, "
        "COALESCE(SUM(CASE WHEN hits>=? THEN 1 ELSE 0 END),0) AS alerting, "
        "COALESCE(SUM(CASE WHEN last_seen>=? THEN 1 ELSE 0 END),0) AS recent "
        "FROM not_found_paths",
        ALERT_HITS, now - DAY_MS) or {}
    summary = dict(summary)
    total = int(summary.get("group_count") or 0)
    pages = max(1, (total + PAGE_SIZE - 1) // PAGE_SIZE)
    page = min(page, pages)
    first_hour = _hour_of(now) - CHART_HOURS + 1
    same_group = (
        "FROM not_found_hours h WHERE h.site=p.site AND h.method=p.method "
        "AND h.path=p.path AND h.hour>=?")
    rows = await d1_all(
        env,
        "SELECT p.site,p.method,p.path,p.hits,p.first_seen,p.last_seen,"
        "p.referrer,p.alerted_at,p.last_client,"
        "(SELECT COALESCE(SUM(h.hits),0) " + same_group + ") AS day_hits,"
        "(SELECT group_concat(h.hour||':'||h.hits) " + same_group
        + ") AS hours FROM not_found_paths p ORDER BY " + SORT_SQL[sort]
        + " LIMIT ? OFFSET ?",
        first_hour, first_hour, PAGE_SIZE, (page - 1) * PAGE_SIZE)
    body = []
    for row in rows or []:
        row = dict(row)
        hits = int(row.get("hits") or 0)
        frequency = _hour_series(row.get("hours"), first_hour)
        day_hits = sum(frequency)
        alerted = int(row.get("alerted_at") or 0)
        if alerted:
            alert = shell.pill("Alerted", "bad")
        elif hits >= ALERT_HITS:
            alert = shell.pill("Pending", "warn")
        else:
            alert = '<span class="muted">%d to go</span>' % (ALERT_HITS - hits)
        body.append(
            "<tr>%s<td>%s</td><td><div style=\"display:flex;"
            'align-items:flex-end;gap:8px">%s'
            '<span class="muted">%s</span></div></td>'
            "<td class=\"mono muted\">%s</td>%s"
            "<td>%s</td><td>%s</td><td>%s</td>"
            '<td class="r"><b>%s</b></td>'
            '<td class="r"><div class="actions" '
            'style="justify-content:flex-end">%s%s</div></td></tr>' % (
                _request_cell(row),
                shell.pill(source_label(row.get("path")))
                + ((" " + shell.pill("Probe", "bad"))
                   if _probe_ban.classify(row.get("path")) else ""),
                shell.sparkline(
                    frequency, "24-hour frequency: %d hit%s"
                    % (day_hits, "" if day_hits == 1 else "s")),
                shell.format_int(day_hits),
                _html_escape(row.get("referrer") or "—"),
                _client_cell(await _open_client(env, row.get("last_client"))),
                shell.time_html(row.get("first_seen"), now),
                shell.time_html(row.get("last_seen"), now),
                alert,
                shell.format_int(hits),
                _ignore_form(row, csrf_field, admin_query, sort, page),
                _clear_form(row, csrf_field, admin_query, sort, page)))
    head = (
        "<th>Request</th><th>Source</th><th>24-hour frequency</th>"
        "<th>Last referrer</th>"
        "<th>Last visitor</th>"
        "<th>First seen</th><th>Last seen</th><th>Alert</th>"
        '<th class="r">Hits</th><th class="r">Actions</th>')
    table = (
        "<table><thead><tr>" + head + "</tr></thead><tbody>"
        + "".join(body) + "</tbody></table>"
        if body else shell.empty("No 404s recorded yet."))
    tabs = shell.tabs(
        [(key, label, _admin_href(admin_query, view=VIEW, sort=key), None)
         for key, label in SORTS.items()],
        sort)
    pager = ""
    if pages > 1:
        pager = (
            '<div class="pager"><span>Page %d of %d</span><div class="actions">'
            '%s%s</div></div>' % (
                page, pages,
                ('<a class="btn btn-sm" href="%s">Previous</a>' % _admin_href(
                    admin_query, view=VIEW, sort=sort, page=page - 1))
                if page > 1 else "",
                ('<a class="btn btn-sm" href="%s">Next</a>' % _admin_href(
                    admin_query, view=VIEW, sort=sort, page=page + 1))
                if page < pages else ""))
    alerting = int(summary.get("alerting") or 0)
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("Groups", shell.format_int(total), "all time")
        + shell.kpi("Hits", shell.format_int(summary.get("hits") or 0),
                    "all time")
        + shell.kpi("At %d+ hits" % ALERT_HITS, shell.format_int(alerting),
                    "all time", tone="warn" if alerting else "")
        + shell.kpi("Seen today", shell.format_int(summary.get("recent") or 0),
                    "last 24 hours")
        + "</div>")
    return (
        shell.page_head(
            "404s",
            "Requests that found nothing on any ForkMesh site, grouped by site, "
            "method and path. A group pings administrators once when it "
            "reaches %d hits. Ignore a path that was deleted on purpose to "
            "stop logging it. Probe marks a path only a scanner asks for; "
            "those count toward an auto ban." % ALERT_HITS,
            crumb="Reliability")
        + kpis
        + await _render_chart(env, first_hour)
        + '<section class="card">' + tabs
        + '<div class="card-t">' + table + "</div>" + pager + "</section>"
        + await _probe_ban.render(
            env,
            lambda row: _row_form(
                "lift_probe_ban", row, ("rule_id",), "Lift",
                "Remove this ban now and forget the visitor's score",
                csrf_field, admin_query, sort, page),
            _client_cell, now)
        + await _render_ignores(env, csrf_field, admin_query, sort, page, now))
