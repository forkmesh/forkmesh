import datetime
import json
import re


def _bind_runtime(runtime):
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


VIEW = "ids"
HOUR_MS = 60 * 60 * 1000
DAY_MS = 24 * HOUR_MS
HEARTBEAT_STALE_MS = 180_000
PAGE_SIZE = 50
MAX_GROUPS = 100
DETAIL_MAX = 4000
RANGES = {
    "day": ("24 hours", DAY_MS, HOUR_MS),
    "week": ("7 days", 7 * DAY_MS, 6 * HOUR_MS),
    "month": ("30 days", 30 * DAY_MS, DAY_MS),
}
SEVERITIES = ("lockdown", "alert", "test", "info")
SEVERITY_TONES = {"lockdown": "bad", "alert": "warn", "test": "info",
                  "info": ""}
PAGED = ("alert", "lockdown", "test")
GROUPS = {"detector": "By detector", "node": "By node"}
GROUP_COLUMNS = {"detector": "detector", "node": "node_id"}
QUARANTINE_LABELS = {
    "self": ("Locked down", "bad"),
    "vote": ("Voted into quarantine", "bad"),
    "release": ("Released", "ok"),
}
_KEY_RE = re.compile(r"^[A-Za-z0-9_.:-]{1,64}$")
_PAGED_SQL = "severity IN ('alert','lockdown','test')"


def _now():
    return int(Date.now())


def _first(params, key):
    values = (params or {}).get(key) or [""]
    return str(values[0] or "")


def _int(value):
    try:
        return int(value or 0)
    except (TypeError, ValueError):
        return 0


def _plural(count, word):
    return "%s %s%s" % (_admin_shell.format_int(count), word,
                        "" if count == 1 else "s")


def _filters(params):
    window = _first(params, "range")
    severity = _first(params, "severity")
    group = _first(params, "group")
    node = _first(params, "node")
    detector = _first(params, "detector")
    page = _first(params, "page")
    return {
        "range": window if window in RANGES else "day",
        "severity": severity if severity in SEVERITIES + ("paged",) else "",
        "group": group if group in GROUPS else "detector",
        "node": node if _KEY_RE.match(node) else "",
        "detector": detector if _KEY_RE.match(detector) else "",
        "page": max(1, int(page)) if page.isdigit() and len(page) <= 6 else 1,
    }


def _href(admin_query, filters, **changes):
    values = dict(filters, **changes)
    return _admin_href(
        admin_query, view=VIEW,
        range=None if values["range"] == "day" else values["range"],
        severity=values["severity"] or None,
        group=None if values["group"] == "detector" else values["group"],
        node=values["node"] or None,
        detector=values["detector"] or None,
        page=values["page"] if values["page"] > 1 else None)


def _where(filters, start):
    clauses = ["created_ms>=?"]
    args = [start]
    if filters["severity"] == "paged":
        clauses.append(_PAGED_SQL)
    elif filters["severity"]:
        clauses.append("severity=?")
        args.append(filters["severity"])
    if filters["node"]:
        clauses.append("node_id=?")
        args.append(filters["node"])
    if filters["detector"]:
        clauses.append("detector=?")
        args.append(filters["detector"])
    return " WHERE " + " AND ".join(clauses), args


def _window(filters, now):
    _label, span, bucket = RANGES[filters["range"]]
    first = (now // bucket) - (span // bucket) + 1
    return first, span // bucket, bucket


def _bucket_label(index, bucket, first):
    moment = datetime.datetime.fromtimestamp(
        (first + index) * bucket / 1000, datetime.timezone.utc)
    if bucket >= DAY_MS:
        return "%s %d" % (_admin_shell.MONTHS[moment.month - 1], moment.day)
    if bucket > HOUR_MS:
        return "%s %d %02d:00" % (_admin_shell.MONTHS[moment.month - 1],
                                  moment.day, moment.hour)
    return "%02d:00" % moment.hour


def _html(body, status=200):
    return Response(body, status=status, headers={
        "content-type": "text/html; charset=utf-8",
        "cache-control": "no-store",
        "x-robots-tag": "noindex, nofollow",
    })


async def admin_handler(env, request, params, csrf_field="", admin_query="",
                        account=""):
    if method_name(request) != "GET":
        return Response("", status=405, headers={
            "allow": "GET", "cache-control": "no-store"})
    shell = _admin_shell
    content = await render(env, params, admin_query)
    url = urlparse(request.url)
    page = shell.render_page(
        "Intrusion detection", content, active=VIEW,
        console=shell.console_base(env),
        theme=shell.theme_from_request(request),
        badges=await shell.nav_badges(env), account=account,
        subpages=shell.subpage_base(env),
        next_path=url.path + (("?" + url.query) if url.query else ""))
    return _html(page)


async def _counts(env, start, end=None):
    sql = (
        "SELECT COUNT(*) AS n,"
        "COALESCE(SUM(CASE WHEN severity='lockdown' THEN 1 ELSE 0 END),0) "
        "AS lockdowns,"
        "COALESCE(SUM(CASE WHEN severity IN ('alert','test') THEN 1 ELSE 0 "
        "END),0) AS alerts,"
        "COALESCE(SUM(CASE WHEN " + _PAGED_SQL + " AND delivered_ms IS NULL "
        "THEN 1 ELSE 0 END),0) AS undelivered,"
        "COUNT(DISTINCT node_id) AS nodes "
        "FROM security_incidents WHERE created_ms>=?")
    args = [start]
    if end is not None:
        sql += " AND created_ms<?"
        args.append(end)
    return dict(await d1_first(env, sql, *args) or {})


async def _nodes(env, start, now):
    beats = {
        str(row.get("node_id") or ""): row
        for row in (dict(item) for item in await d1_all(
            env, "SELECT node_id,beat_ms,state FROM ids_heartbeats") or [])}
    quarantine = {
        str(row.get("node_id") or ""): row
        for row in (dict(item) for item in await d1_all(
            env,
            "SELECT q.node_id,q.kind,q.signer,q.created_ms,q.reunsealed_ms "
            "FROM cluster_quarantine q WHERE q.created_ms=("
            "SELECT MAX(created_ms) FROM cluster_quarantine "
            "WHERE node_id=q.node_id)") or [])}
    incidents = {
        str(row.get("node_id") or ""): row
        for row in (dict(item) for item in await d1_all(
            env,
            "SELECT node_id,COUNT(*) AS n,MAX(created_ms) AS last,"
            "COALESCE(SUM(CASE WHEN " + _PAGED_SQL + " THEN 1 ELSE 0 END),0) "
            "AS paged FROM security_incidents WHERE created_ms>=? "
            "GROUP BY node_id", start) or [])}
    names = sorted((set(beats) | set(quarantine) | set(incidents)) - {""})
    nodes = []
    for name in names:
        beat = beats.get(name) or {}
        beat_ms = _int(beat.get("beat_ms"))
        state = str(beat.get("state") or "")
        if not beat:
            health = ("No heartbeat", "")
        elif state == "lockdown":
            health = ("Lockdown", "bad")
        elif now - beat_ms > HEARTBEAT_STALE_MS:
            health = ("Silent", "bad")
        else:
            health = ("Normal", "ok")
        nodes.append({"node": name, "beat_ms": beat_ms, "health": health,
                      "quarantine": quarantine.get(name),
                      "incidents": incidents.get(name) or {}})
    return nodes


def _quarantine_cell(row, now):
    if not row:
        return '<span class="muted">none</span>'
    label, tone = QUARANTINE_LABELS.get(
        str(row.get("kind") or ""), (str(row.get("kind") or "record"), ""))
    by = ""
    if row.get("kind") == "vote":
        by = ' <span class="muted">by %s</span>' % _html_escape(
            row.get("signer") or "")
    elif row.get("kind") == "release" and not _int(row.get("reunsealed_ms")):
        by = " " + _admin_shell.pill("not unsealed yet", "warn")
    return "%s%s %s" % (_admin_shell.pill(label, tone), by,
                        _admin_shell.time_html(row.get("created_ms"), now))


def _render_nodes(nodes, filters, admin_query, now):
    shell = _admin_shell
    rows = []
    for node in nodes:
        counts = node["incidents"]
        rows.append(
            '<tr><td class="mono"><a href="%s">%s</a></td><td>%s</td>'
            "<td>%s</td><td>%s</td>"
            '<td class="r">%s</td><td class="r">%s</td><td>%s</td></tr>' % (
                _href(admin_query, filters, node=node["node"], page=1),
                _html_escape(node["node"]),
                shell.pill(*node["health"]),
                shell.time_html(node["beat_ms"], now) if node["beat_ms"]
                else '<span class="muted">never</span>',
                _quarantine_cell(node["quarantine"], now),
                shell.format_int(counts.get("n") or 0),
                shell.format_int(counts.get("paged") or 0),
                shell.time_html(counts.get("last"), now) if counts.get("last")
                else '<span class="muted">none</span>'))
    return shell.card(
        title="Nodes",
        description="Each core's IDS heart-beats every 30 seconds; a "
        "heartbeat older than 3 minutes is silent. Runners have no "
        "heartbeat row and appear when they report an incident in the "
        "chosen window.",
        table=shell.table(
            "<th>Node</th><th>IDS</th><th>Last heartbeat</th>"
            "<th>Latest quarantine record</th>"
            '<th class="r">Incidents</th><th class="r">Paged</th>'
            "<th>Last incident</th>",
            rows, "No IDS has reported yet."),
        cls="ids-nodes")


async def _render_chart(env, filters, where, args, now):
    shell = _admin_shell
    first, count, bucket = _window(filters, now)
    rows = await d1_all(
        env,
        "SELECT created_ms/? AS b,COUNT(*) AS n,"
        "COALESCE(SUM(CASE WHEN " + _PAGED_SQL + " THEN 1 ELSE 0 END),0) "
        "AS paged FROM security_incidents" + where + " GROUP BY b",
        bucket, *args)
    every = [0] * count
    paged = [0] * count
    for row in rows or []:
        row = dict(row)
        index = _int(row.get("b")) - first
        if 0 <= index < count:
            every[index] += _int(row.get("n"))
            paged[index] += _int(row.get("paged"))
    total = sum(every)
    label = RANGES[filters["range"]][0]
    if not total:
        body = shell.empty("No incidents in the last %s." % label)
    else:
        labels = [_bucket_label(i, bucket, first) for i in range(count)]
        body = (
            shell.line_chart(
                labels, [(every, "accent"), (paged, "accent")], height=200,
                bars=True, title="IDS incidents, last %s" % label)
            + '<div class="legend"><span><i class="soft"></i>All incidents'
            '</span><span><i class="box"></i>Alerts and lockdowns</span>'
            "</div>")
    return shell.card(
        body, title="Incidents, last %s" % label,
        description="%s, %s paged administrators" % (
            _plural(total, "incident"), shell.format_int(sum(paged))),
        cls="ids-chart")


def _severity_cell(counts):
    shell = _admin_shell
    parts = []
    for severity in SEVERITIES:
        value = _int(counts.get(severity))
        if value:
            parts.append(shell.pill("%s %s" % (shell.format_int(value),
                                               severity),
                                    SEVERITY_TONES[severity]))
    return " ".join(parts)


async def _render_groups(env, filters, where, args, admin_query, now):
    shell = _admin_shell
    column = GROUP_COLUMNS[filters["group"]]
    other = "node_id" if column == "detector" else "detector"
    first, count, bucket = _window(filters, now)
    severity_sums = ",".join(
        "COALESCE(SUM(CASE WHEN severity='%s' THEN 1 ELSE 0 END),0) AS %s"
        % (severity, severity) for severity in SEVERITIES)
    rows = await d1_all(
        env,
        "SELECT " + column + " AS k,COUNT(*) AS n,MAX(created_ms) AS last,"
        "summary,COUNT(DISTINCT " + other + ") AS spread," + severity_sums
        + " FROM security_incidents" + where + " GROUP BY " + column
        + " ORDER BY n DESC, last DESC LIMIT ?",
        *args, MAX_GROUPS)
    series = {}
    for row in await d1_all(
            env,
            "SELECT " + column + " AS k,created_ms/? AS b,COUNT(*) AS n "
            "FROM security_incidents" + where + " GROUP BY k,b",
            bucket, *args) or []:
        row = dict(row)
        index = _int(row.get("b")) - first
        if 0 <= index < count:
            series.setdefault(str(row.get("k") or ""), [0] * count)[index] += (
                _int(row.get("n")))
    peak = 0
    groups = [dict(row) for row in rows or []]
    for row in groups:
        peak = max(peak, _int(row.get("n")))
    body = []
    for row in groups:
        key = str(row.get("k") or "")
        total = _int(row.get("n"))
        frequency = series.get(key) or [0] * count
        link = _href(admin_query, filters, page=1,
                     **{filters["group"]: key})
        body.append(
            '<tr><td class="mono"><a href="%s">%s</a></td><td>%s</td>'
            '<td class="nowrap">%s</td><td>%s</td>'
            '<td class="muted nowrap">%s</td><td>%s</td>'
            '<td class="r"><b>%s</b></td></tr>' % (
                link, _html_escape(key), _severity_cell(row),
                shell.sparkline(frequency, "Frequency: %s" % _plural(
                    total, "incident")),
                shell.hbar(100.0 * total / peak if peak else 0,
                           "bad" if _int(row.get("lockdown")) else ""),
                _plural(_int(row.get("spread")),
                        "node" if column == "detector" else "detector"),
                _html_escape(str(row.get("summary") or "")[:160]),
                shell.format_int(total)))
    tabs = shell.tabs(
        [(key, label, _href(admin_query, filters, group=key, page=1), None)
         for key, label in GROUPS.items()],
        filters["group"])
    table = shell.table(
        "<th>%s</th><th>Severity</th><th>Frequency</th><th>Share</th>"
        "<th>Spread</th><th>Latest summary</th>"
        '<th class="r">Incidents</th>' % (
            "Detector" if column == "detector" else "Node"),
        body, "No incidents match.")
    return ('<section class="card ids-groups">' + tabs
            + '<div class="card-t">' + table + "</div></section>")


def _detail_html(text):
    raw = str(text or "")
    if not raw:
        return ""
    try:
        pretty = json.dumps(json.loads(raw), indent=2, sort_keys=True)
    except ValueError:
        pretty = raw
    return (
        '<details class="more-text"><summary>Detail</summary>'
        '<pre class="mono" style="white-space:pre-wrap;margin:0;'
        'max-width:640px">%s</pre></details>'
        % _html_escape(pretty[:DETAIL_MAX]))


def _delivery_cell(row):
    shell = _admin_shell
    severity = str(row.get("severity") or "")
    if severity not in PAGED:
        return '<span class="muted">logged</span>'
    state = str(row.get("email_state") or "")
    email = (' <span class="muted">email %s</span>' % _html_escape(state)
             if state else "")
    if _int(row.get("delivered_ms")):
        return shell.pill("Paged", "ok") + email
    return shell.pill("Pending", "warn") + email


async def _render_log(env, filters, where, args, admin_query, now):
    shell = _admin_shell
    total = _int(dict(await d1_first(
        env, "SELECT COUNT(*) AS n FROM security_incidents" + where, *args)
        or {}).get("n"))
    pages = max(1, (total + PAGE_SIZE - 1) // PAGE_SIZE)
    page = min(filters["page"], pages)
    rows = await d1_all(
        env,
        "SELECT id,node_id,created_ms,severity,detector,summary,detail,"
        "delivered_ms,email_state FROM security_incidents" + where
        + " ORDER BY created_ms DESC, id LIMIT ? OFFSET ?",
        *args, PAGE_SIZE, (page - 1) * PAGE_SIZE)
    body = []
    for row in rows or []:
        row = dict(row)
        severity = str(row.get("severity") or "")
        body.append(
            "<tr><td>%s</td>"
            '<td class="mono"><a href="%s">%s</a></td><td>%s</td>'
            '<td class="mono"><a href="%s">%s</a></td>'
            '<td>%s<div class="muted mono" style="font-size:11px">%s</div>%s'
            "</td><td>%s</td></tr>" % (
                shell.time_html(row.get("created_ms"), now),
                _href(admin_query, filters, node=row.get("node_id") or "",
                      page=1),
                _html_escape(row.get("node_id") or ""),
                shell.pill(severity, SEVERITY_TONES.get(severity, "")),
                _href(admin_query, filters,
                      detector=row.get("detector") or "", page=1),
                _html_escape(row.get("detector") or ""),
                _html_escape(row.get("summary") or ""),
                _html_escape(row.get("id") or ""),
                _detail_html(row.get("detail")),
                _delivery_cell(row)))
    pager = ""
    if pages > 1:
        pager = (
            '<div class="pager"><span>Page %d of %d</span><div class="actions">'
            "%s%s</div></div>" % (
                page, pages,
                ('<a class="btn btn-sm" href="%s">Newer</a>' % _href(
                    admin_query, filters, page=page - 1)) if page > 1 else "",
                ('<a class="btn btn-sm" href="%s">Older</a>' % _href(
                    admin_query, filters, page=page + 1))
                if page < pages else ""))
    return shell.card(
        title="Incident log",
        description="%s, newest first" % _plural(total, "incident"),
        table=shell.table(
            "<th>Time</th><th>Node</th><th>Severity</th><th>Detector</th>"
            "<th>Summary</th><th>Delivery</th>",
            body, "No incidents match."),
        footer=pager, cls="ids-log")


def _render_filters(filters, admin_query):
    shell = _admin_shell
    ranges = shell.tabs(
        [(key, label, _href(admin_query, filters, range=key, page=1), None)
         for key, (label, _span, _bucket) in RANGES.items()],
        filters["range"])
    levels = [("", "All severities"), ("paged", "Alerts and lockdowns")] + [
        (severity, severity.capitalize()) for severity in SEVERITIES]
    severity = shell.tabs(
        [(key or "all", label,
          _href(admin_query, filters, severity=key, page=1), None)
         for key, label in levels],
        filters["severity"] or "all")
    chips = []
    for key, label in (("node", "Node"), ("detector", "Detector")):
        if filters[key]:
            chips.append(
                '<a class="btn btn-sm" href="%s" title="Remove this filter">'
                "%s: %s ×</a>" % (
                    _href(admin_query, filters, page=1, **{key: ""}),
                    label, _html_escape(filters[key])))
    return (
        '<section class="card ids-filters">' + ranges + severity
        + ('<div class="card-b actions">%s</div>' % "".join(chips)
           if chips else "")
        + "</section>")


async def render(env, params, admin_query="", now=None):
    shell = _admin_shell
    now = int(now or _now())
    filters = _filters(params)
    label, span, _bucket = RANGES[filters["range"]]
    first, _count, bucket = _window(filters, now)
    start = first * bucket
    current = await _counts(env, start)
    previous = await _counts(env, start - span, start)
    nodes = await _nodes(env, start, now)
    beating = [node for node in nodes if node["beat_ms"]]
    healthy = [node for node in beating if node["health"][1] == "ok"]
    lockdowns = _int(current.get("lockdowns"))
    undelivered = _int(current.get("undelivered"))
    window = "last " + label
    kpis = (
        '<div class="grid cols-4">'
        + shell.kpi("Incidents", shell.format_int(current.get("n")), window,
                    change=shell.delta_pill(current.get("n"), previous.get("n"),
                                            good_when_up=False))
        + shell.kpi("Alerts", shell.format_int(current.get("alerts")),
                    window + (", %s not yet paged" % shell.format_int(
                        undelivered) if undelivered else ""),
                    change=shell.delta_pill(current.get("alerts"),
                                            previous.get("alerts"),
                                            good_when_up=False),
                    tone="warn" if _int(current.get("alerts")) else "")
        + shell.kpi("Lockdowns", shell.format_int(lockdowns), window,
                    tone="bad" if lockdowns else "")
        + shell.kpi("IDS reporting",
                    "%d / %d" % (len(healthy), len(beating)),
                    "core heartbeats",
                    tone="bad" if len(healthy) < len(beating) else "")
        + "</div>")
    where, args = _where(filters, start)
    return (
        shell.page_head(
            "Intrusion detection",
            "Incidents reported by the IDS on every core and runner "
            "(docs/IDS.md): node health, incidents over time, grouped by "
            "detector or node, and the full incident log. Alerts and "
            "lockdowns page administrators; info incidents are only logged.",
            crumb="Security")
        + kpis
        + _render_nodes(nodes, filters, admin_query, now)
        + _render_filters(filters, admin_query)
        + await _render_chart(env, filters, where, args, now)
        + await _render_groups(env, filters, where, args, admin_query, now)
        + await _render_log(env, filters, where, args, admin_query, now))
