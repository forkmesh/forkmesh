from __future__ import annotations

import math
import re

from . import repo_ssr

DOT = {"green": "bg-success", "yellow": "bg-attention", "red": "bg-destructive",
       "unknown": "bg-muted-foreground/40"}
OVERALL_WORD = {"green": "Mirror goal met", "yellow": "At least half of the mirror goal",
                "red": "Below half of the mirror goal", "unknown": "No mirrors checked yet"}
WORD = {"green": "Healthy", "yellow": "Behind", "red": "Unreachable",
        "unknown": "Not checked"}
HTTPS_RE = re.compile(r"https://[^\s\"'<>]+")
ESCAPES = {"&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#039;"}


def escape(value):
    return "".join(ESCAPES.get(char, char) for char in str("" if value is None else value))


def _number(value):
    if isinstance(value, bool):
        return 1 if value else 0
    try:
        number = float(value)
    except (TypeError, ValueError):
        return 0
    return number if math.isfinite(number) else 0


def _int_text(value):
    return str(int(value)) if float(value).is_integer() else repr(float(value))


def row_html(row, now=None):
    status = row.get("status") if row.get("status") in DOT else "unknown"
    label = str(row.get("label") or row.get("host") or "")
    url = str(row.get("url") or "")
    href = url if HTTPS_RE.fullmatch(url) else ""
    word = WORD[status]
    detail = str(row.get("statusLabel") or word)
    at = _number((row.get("lastCheck") or {}).get("at"))
    checked = (" · checked %s" % repo_ssr.time_ago(at, now)
               if row.get("id") != "self" and at > 0 else "")
    if href:
        name = ('<a href="%s" target="_blank" rel="noopener noreferrer nofollow" '
                'class="dashboard-accent-link block min-w-0 truncate">%s</a>'
                % (escape(href), escape(label)))
    else:
        name = ('<span class="block min-w-0 truncate text-foreground" title="%s">%s</span>'
                % (escape(url), escape(label)))
    host = row.get("host")
    host = "%s · " % escape(host) if host and host != label else ""
    remove = ""
    if row.get("canRemove") and not row.get("builtin"):
        remove = ('<button type="button" data-mirror-quota-remove="%s" aria-label="Remove %s" '
                  'class="shrink-0 text-[11px] text-muted-foreground hover:text-destructive">Remove'
                  '</button>' % (escape(row.get("id")), escape(label)))
    return ('<li data-mirror-quota-row="%s" data-mirror-status="%s" class="flex '
            'items-center gap-2 rounded-md px-2 py-1.5 text-xs"><span role="img" aria-label="%s" title="%s" class="h-2 w-2 shrink-0 '
            'rounded-full %s"></span><span class="min-w-0 flex-1">%s<span class="block truncate '
            'text-[11px] text-muted-foreground">%s%s%s</span></span>%s</li>' % (
                escape(row.get("id") or ""), status, word, word, DOT[status], name, host,
                escape(detail), escape(checked), remove))


def overall_status(healthy, target):
    if target <= 0:
        return "green" if healthy > 0 else "unknown"
    if healthy >= target:
        return "green"
    return "yellow" if healthy * 2 >= target else "red"


def card_html(data, viewer=None, now=None, help_href=""):
    if not isinstance(data, dict) or not isinstance(data.get("mirrors"), list):
        return ""
    target = max(0, _number(data.get("target")))
    healthy = max(0, _number(data.get("healthy")))
    registered = max(0, _number(data.get("registered")))
    can_set = bool((viewer or {}).get("canSetTarget"))
    if not target and not registered and not can_set:
        return ""
    count = ("%s / %s healthy" % (_int_text(healthy), _int_text(target)) if target
             else "%s healthy" % _int_text(healthy))
    overall = overall_status(healthy, target)
    bar = ""
    if target:
        percent = min(100, math.floor(healthy * 100 / target + 0.5))
        bar = ('<div role="progressbar" aria-label="Healthy mirrors" aria-valuemin="0" '
               'aria-valuemax="%s" aria-valuenow="%s" class="mt-2 h-1.5 w-full '
               'overflow-hidden rounded-full bg-muted-foreground/20"><span class="block '
               'h-full rounded-full %s" style="width:%s%%"></span></div>' % (
                   _int_text(target), _int_text(min(healthy, target)), DOT[overall],
                   _int_text(percent)))
    if help_href and target and overall != "green":
        missing = max(0, target - healthy)
        bar += ('<p data-mirror-quota-help class="mt-1.5 text-[11px] text-muted-foreground">Needs '
                '%s more mirror%s &middot; <a href="%s" class="dashboard-accent-link">How to help '
                '&rarr;</a></p>' % (_int_text(missing), "" if missing == 1 else "s",
                                     escape(help_href)))
    rows = "".join(row_html(row if isinstance(row, dict) else {}, now)
                   for row in data["mirrors"])
    goal = ""
    if can_set:
        goal = ('<form data-mirror-quota-target-form class="mt-2 flex flex-wrap items-center '
                'gap-2 text-[11px] text-muted-foreground"><label class="inline-flex '
                'items-center gap-1.5">%s<input name="target" type="number" '
                'inputmode="numeric" min="0" max="%s" value="%s" aria-label="Healthy mirrors '
                'wanted" class="h-control-sm w-14 rounded-md border border-border bg-background '
                'px-2 font-mono text-control text-foreground outline-none focus:border-primary" '
                '/></label><button type="submit" class="fm-btn fm-btn-sm">Set target</button>'
                '<span data-mirror-quota-target-status role="status" class="min-w-0 truncate">'
                '</span></form>' % (
                    "Goal" if target else "Set a mirror goal",
                    _int_text(_number(data.get("maxTarget")) or 100),
                    _int_text(target or 10)))
    summary = OVERALL_WORD[overall]
    return ('<div data-mirror-quota-overall="%s" class="flex items-center justify-between '
            'gap-3"><h4 class="inline-flex items-center gap-1.5 text-xs font-semibold '
            'text-foreground"><span role="img" aria-label="%s" title="%s" class="h-2 w-2 '
            'shrink-0 rounded-full %s"></span>Mirrors</h4><span data-mirror-quota-count '
            'class="font-mono text-[11px] text-muted-foreground">%s</span></div>%s<ul '
            'class="-mx-2 mt-2 grid">%s</ul><div class="mt-2 flex flex-wrap items-center '
            'gap-2"><button type="button" data-mirror-quota-add class="fm-btn fm-btn-sm">Add a '
            'mirror</button><span data-mirror-quota-status role="status" class="min-w-0 '
            'truncate text-[11px] text-destructive"></span></div>%s' % (
                overall, summary, summary, DOT[overall], count, bar, rows, goal))


def section_html(data, now=None, help_href=""):
    inner = card_html(data, None, now, help_href)
    return ('<section data-repo-mirror-quota aria-label="Mirrors" class="mt-3 border-t '
            'border-border pt-3"%s>%s</section>' % ("" if inner else " hidden", inner))
