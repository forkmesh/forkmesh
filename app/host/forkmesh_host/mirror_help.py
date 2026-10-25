from __future__ import annotations

import datetime
import html
import math
import urllib.parse

HEADLINE = "This forum needs your help"
ACTION = "Run a mirror or join the federation"
TITLE = "Help keep this forum reachable"
START_URL = "https://forkmesh.com/start"
INSTALLER_URL = "https://forkmesh.com/install-instance.sh"
FORGE_SYNC_DOCS = "https://forkmesh.com/docs/forge-sync/"
MIRROR_DOCS = "https://forkmesh.com/docs/mirror-quota/"
STATUS_WORD = {"green": "goal met", "yellow": "behind", "red": "urgent"}
ROW_WORD = {"green": "Healthy", "yellow": "Behind", "red": "Unreachable",
            "unknown": "Not checked"}
STYLE = (
    "<style>.st-green{color:var(--g)}.st-yellow{color:var(--a)}.st-red{color:var(--d)}"
    ".st-unknown{color:var(--m)}ol.w>li{margin:1.2em 0}pre{white-space:pre-wrap}"
    "td{vertical-align:top}</style>")


def esc(value):
    return html.escape("" if value is None else str(value), quote=True)


def _count(value):
    if isinstance(value, bool):
        return 0
    try:
        number = float(value)
    except (TypeError, ValueError):
        return 0
    return int(number) if math.isfinite(number) and number > 0 else 0


def valid(quota):
    return isinstance(quota, dict) and quota.get("ok") is not False \
        and isinstance(quota.get("mirrors"), list)


def overall(healthy, target):
    if target <= 0:
        return "green" if healthy > 0 else "red"
    if healthy >= target:
        return "green"
    return "yellow" if healthy * 2 >= target else "red"


def help_state(quota, home=False):
    if valid(quota):
        target, healthy = _count(quota.get("target")), _count(quota.get("healthy"))
        if target and healthy < target:
            return {"healthy": healthy, "target": target, "known": True,
                    "status": overall(healthy, target)}
        return None
    if home:
        return {"healthy": 0, "target": 0, "known": False, "status": "red"}
    return None


def count_text(state):
    if not state or not state["known"]:
        return ""
    return "%d of %d mirrors running." % (state["healthy"], state["target"])


def mirror_href(base):
    return base + "/mirror"


def banner_lite(base, state):
    if not state:
        return ""
    count = count_text(state)
    return ('<p class="help"><b>%s</b>%s <a href="%s">%s &rarr;</a></p>' % (
        HEADLINE, " &mdash; " + esc(count) if count else ".", esc(mirror_href(base)), ACTION))


def banner_home(base, state):
    if not state:
        return ""
    count = count_text(state)
    return ('<aside class="help big" data-forum-help><p><b>%s</b>%s</p><p>Censors block '
            'single domains and addresses. Every independent copy of this forum is another '
            'door.</p><p><a class="go" href="%s">%s &rarr;</a></p></aside>' % (
                HEADLINE, " &mdash; " + esc(count) if count else ".", esc(mirror_href(base)),
                ACTION))


def banner_dashboard(base, healthy, target):
    return ('<div data-repo-mirror-help class="flex min-w-0 items-center gap-2 rounded-t-md '
            'border-b border-attention/40 bg-attention/10 px-3 py-1.5 text-xs text-foreground">'
            '<span aria-hidden="true" class="h-2 w-2 shrink-0 rounded-full bg-attention"></span>'
            '<span class="min-w-0 flex-1 truncate"><b class="font-semibold">%s</b> &mdash; %d '
            'of %d mirrors running. <a href="%s" class="dashboard-accent-link font-semibold">'
            '%s &rarr;</a></span><button type="button" data-repo-mirror-help-dismiss '
            'aria-label="Dismiss" class="shrink-0 text-muted-foreground hover:text-foreground">'
            '&times;</button></div>' % (HEADLINE, healthy, target, esc(mirror_href(base)),
                                        ACTION))


def dashboard_banner_for(base, quota):
    state = help_state(quota)
    return banner_dashboard(base, state["healthy"], state["target"]) if state else ""


def subtitle(state):
    if not state:
        return ""
    count = count_text(state)
    return "%s%s %s." % (HEADLINE, ": " + count if count else ".", ACTION)


def _when(at):
    try:
        moment = float(at)
    except (TypeError, ValueError):
        return ""
    if not math.isfinite(moment) or moment <= 0:
        return ""
    if moment > 1e12:
        moment /= 1000
    try:
        return datetime.datetime.fromtimestamp(moment, datetime.timezone.utc).strftime(
            "%Y-%m-%d %H:%MZ")
    except (OverflowError, OSError, ValueError):
        return ""


def mirror_rows(quota):
    rows = []
    for row in quota.get("mirrors") or []:
        if not isinstance(row, dict):
            continue
        status = row.get("status") if row.get("status") in ROW_WORD else "unknown"
        check = row.get("lastCheck") if isinstance(row.get("lastCheck"), dict) else {}
        when = "now" if row.get("id") == "self" else (_when(check.get("at")) or "never")
        rows.append('<tr><td>%s<br><code>%s</code></td><td>%s</td><td class="st-%s">%s</td>'
                    '<td>%s</td></tr>' % (
                        esc(row.get("label") or row.get("host") or "mirror"),
                        esc(row.get("url") or ""), esc(row.get("host") or ""), status,
                        esc(row.get("statusLabel") or ROW_WORD[status]), esc(when)))
    return rows


def block(text):
    return "<pre><code>%s</code></pre>" % esc(text)


def mirror_page(owner, repo, base, origin, quota, state):
    name = "%s/%s" % (owner, repo)
    clone = str((quota or {}).get("cloneUrl") or "") if valid(quota) else ""
    if not clone.startswith("https://"):
        clone = "%s%s.git" % (origin, base)
    folder = "/srv/git/%s.git" % "".join(
        char for char in repo if char.isalnum() or char in "._-")
    parts = ['<nav><a href="%s/lite">%s</a></nav>' % (esc(base), esc(name)),
             "<h1>%s</h1>" % TITLE,
             "<p>Censors block single domain names and IP addresses. Every independent copy "
             "of this forum is another door that has to be found and closed. A git clone of "
             "<code>%s</code> carries every thread and comment (they live in "
             "<code>.forkmesh/issues</code>), each signed, so anyone can check that a copy is "
             "complete and unchanged.</p>" % esc(name)]
    if valid(quota):
        target, healthy = _count(quota.get("target")), _count(quota.get("healthy"))
        status = overall(healthy, target)
        parts.append('<h2>Status: <span class="st-%s">%s</span></h2><p>%s</p>' % (
            status, STATUS_WORD[status],
            "%d of %d healthy mirrors." % (healthy, target) if target
            else "%d healthy mirror%s." % (healthy, "" if healthy == 1 else "s")))
        rows = mirror_rows(quota)
        if rows:
            parts.append("<table><tr><th>Mirror</th><th>Host</th><th>Status</th>"
                         "<th>Last check</th></tr>%s</table>" % "".join(rows))
    else:
        parts.append("<h2>Status</h2><p>The mirror count is not available right now.</p>")
    if state and state["known"]:
        more = state["target"] - state["healthy"]
        parts.append("<p><b>%d more mirror%s needed.</b></p>" % (more, "" if more == 1 else "s"))
    settings = ('{\n  "projectName": "%s mirror",\n  "domain": "forum.example.org",\n'
                '  "adminUsername": "admin",\n  "adminEmail": "you@example.org",\n'
                '  "adminPassword": "a long new password",\n  "ownerType": "org",\n'
                '  "ownerName": "%s"\n}' % (repo, owner))
    register = ('{"url": "https://mirror.example.org/%s.git", "label": "My mirror"}'
                % repo)
    parts.append('<h2>Ways to help</h2><ol class="w">')
    parts.append(
        "<li><h3>Run a git mirror in two commands</h3><p>On any server with git:</p>%s"
        "<p>Keep it fresh every 15 minutes with cron:</p>%s<p>Then serve "
        "<code>/srv/git</code> with any static HTTPS web server, for example:</p>%s"
        "<p>Readers can then clone <code>https://mirror.example.org/%s.git</code>. Register "
        "it (step 4) so it is checked and counted.</p></li>" % (
            block("git clone --mirror %s %s\ngit -C %s update-server-info" % (
                clone, folder, folder)),
            block("*/15 * * * * git -C %s remote update --prune && git -C %s "
                  "update-server-info" % (folder, folder)),
            block("caddy file-server --root /srv/git --domain mirror.example.org"),
            esc(repo)))
    parts.append(
        "<li><h3>Join the federation: run your own ForkMesh instance</h3><p>An instance "
        "serves the forum with its issues, feeds and lite pages under your own domain, and "
        "keeps itself up to date. On a fresh Debian 13 or Ubuntu 24.04 server (1 GB RAM), "
        "save a <code>settings.json</code> like this:</p>%s<p>and run:</p>%s<p>Then copy the "
        "forum into it with the mirror mode of <a href=\"%s\">forge sync</a>. Prefer a "
        "guided setup? <a href=\"%s\">Start your own instance</a> connects to your server "
        "and runs the installer for you.</p></li>" % (
            block(settings),
            block("curl -fsSL %s -o install-instance.sh\nsudo bash install-instance.sh "
                  "--config settings.json" % INSTALLER_URL),
            FORGE_SYNC_DOCS, START_URL))
    parts.append(
        "<li><h3>Publish a static read-only copy</h3><p>A copy of the threads as plain HTML "
        "pages can live on GitHub Pages, Codeberg Pages or any other static host, where it "
        "costs nothing to keep online. A tool that exports the forum this way is on its way; "
        "until then a git mirror (step 1) keeps everything needed to build one.</p></li>")
    parts.append(
        '<li><h3>Register your mirror</h3><p><a href="/login?next=%s">Sign in</a>, open '
        '<a href="%s">the repository page</a> and press <b>Add a mirror</b> in the Mirrors '
        "card. It is checked about every 15 minutes and counted above once it is up to "
        "date. Programs can register it too, as a signed-in account:</p>%s<p>sent as "
        "<code>POST /api/repo/%s/%s/mirror-quota</code> (<a href=\"%s\">details</a>).</p>"
        "</li></ol>" % (
            esc(urllib.parse.quote(base, safe="/")), esc(base), block(register),
            esc(urllib.parse.quote(owner, safe="")), esc(urllib.parse.quote(repo, safe="")),
            MIRROR_DOCS))
    note = "<p>One server, one domain or one hosting company is a single point of failure."
    parts.append("<h2>Why we're asking</h2>%s Readers on DNS tunnels and filtered networks "
                 "need several independent places to reach it.</p>" % note)
    parts.append('<hr><nav><a href="%s/lite">Threads</a> &middot; <a href="%s/issues.atom">'
                 'Feed</a> &middot; <a href="%s">Full view</a></nav>' % (
                     esc(base), esc(base), esc(base)))
    return "".join(parts)
