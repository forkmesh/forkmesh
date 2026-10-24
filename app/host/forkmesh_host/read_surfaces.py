from __future__ import annotations

import asyncio
import datetime
import hashlib
import html
import json
import re
import time
import urllib.parse

from . import repo_ssr_issue as issue_md

OWNER = r"([A-Za-z0-9][A-Za-z0-9_-]{0,63})"
REPO = r"([A-Za-z0-9._-]{1,100})"
NUMBER = r"([1-9]\d{0,8})"
ROUTES = (
    ("feed", re.compile(r"^/%s/%s/(issues|activity)\.atom$" % (OWNER, REPO))),
    ("thread-feed", re.compile(r"^/%s/%s/issues/%s\.atom$" % (OWNER, REPO, NUMBER))),
    ("label-feed", re.compile(r"^/%s/%s/labels/([^/]{1,300})\.atom$" % (OWNER, REPO))),
    ("lite", re.compile(r"^/%s/%s/lite/?$" % (OWNER, REPO))),
    ("lite-thread", re.compile(r"^/%s/%s/lite/issues/%s/?$" % (OWNER, REPO, NUMBER))),
)
LITE_HOME_RE = re.compile(r"^/lite/?$")
ISSUES_DIR = ".forkmesh/issues"
ATTACHMENTS = ".forkmesh/attachments/issues/%d.json"
TAG_DATE = "2026"
FEED_LIMIT = 50
FEED_LIMIT_MAX = 200
FEED_BODY_CHARS = 32_000
PAGE_SIZE = 50
HOME_THREADS = 20
HOME_LABELS = 40
HOME_SECONDS = 60
LIST_LABELS = 60
BLOB_BATCH = 20
HEAD_FRESH_SECONDS = 5.0
INSTANCE_FRESH_SECONDS = 60.0
WORKER_BUDGET_SECONDS = 0.5
QUOTA_BUDGET_SECONDS = 0.15
CACHE_BYTES = 24 * 1024 * 1024
CACHE_CONTROL = "public, max-age=60"
ATOM_TYPE = "application/atom+xml; charset=utf-8"
HTML_TYPE = "text/html; charset=utf-8"
CSP = ("default-src 'none'; img-src 'self' data:; style-src 'unsafe-inline'; "
       "base-uri 'none'; form-action 'self'; frame-ancestors 'none'")
XML_BAD = re.compile("[\x00-\x08\x0b\x0c\x0e-\x1f\ufffe\uffff\ud800-\udfff]")
STRIP_ATTRS = re.compile(
    r'\s(?:class|data-(?!repo-svg-src)[a-z0-9-]+|decoding|target|referrerpolicy)="[^"]*"')
SVG_SRC = re.compile(r'\sdata-repo-svg-src="')
IMG_TAG = re.compile(r"<img\b([^>]*?)\s*/?>", re.I)
VIDEO_TAG = re.compile(r'<video\b[^>]*?\ssrc="([^"]*)"[^>]*>\s*</video>', re.I)
ROOT_URL = re.compile(r'(\s(?:href|src))="/(?!/)')
FORGE_PREFIX = re.compile(r"^forge-[a-z]+-[a-z-]*?(?=\d+$)")
ANCHOR_SAFE = re.compile(r"[^A-Za-z0-9_-]")
LABEL_MAX = 100

STYLE = (
    "<style>:root{color-scheme:light dark;--f:#1f2328;--b:#fff;--m:#59636e;--l:#0969da;"
    "--r:#d1d9e0;--c:#f6f8fa}@media(prefers-color-scheme:dark){:root{--f:#e6edf3;--b:#0d1117;"
    "--m:#8b949e;--l:#4493f8;--r:#30363d;--c:#161b22}}body{max-width:48em;margin:0 auto;"
    "padding:.5em 1em 2em;font:16px/1.55 system-ui,-apple-system,Segoe UI,sans-serif;"
    "color:var(--f);background:var(--b);overflow-wrap:anywhere}a{color:var(--l)}"
    "h1{font-size:1.4em;line-height:1.3}h2{font-size:1.15em}.m,article>p:first-child,"
    "nav{color:var(--m);font-size:.875em}ul.t{list-style:none;padding:0}ul.t li{padding:.4em 0;"
    "border-top:1px solid var(--r)}article{border-top:1px solid var(--r);margin-top:1em}"
    "pre{overflow:auto;padding:.6em;background:var(--c);font-size:.85em}code{font-size:.9em}"
    "blockquote{margin:0;padding-inline-start:1em;border-inline-start:3px solid var(--r);"
    "color:var(--m)}img{max-width:100%;height:auto}table{border-collapse:collapse;"
    "display:block;overflow:auto}td,th{border:1px solid var(--r);padding:.2em .5em}"
    "hr{border:0;border-top:1px solid var(--r)}</style>")


def target(scope):
    if scope.get("method", "GET").upper() not in ("GET", "HEAD"):
        return None
    raw = scope.get("raw_path")
    path = raw.decode("latin-1") if raw else (scope.get("path") or "")
    path = path.split("?", 1)[0]
    if LITE_HOME_RE.match(path):
        return ("lite-home",)
    if path == "/":
        return ("home",)
    from .repo_ssr import RESERVED_OWNERS
    for kind, pattern in ROUTES:
        match = pattern.match(path)
        if not match:
            continue
        owner, repo = match.group(1), match.group(2)
        if owner.lower() in RESERVED_OWNERS or repo in (".", "..") \
                or repo.endswith(".git"):
            return None
        rest = match.groups()[2:]
        if kind == "label-feed":
            try:
                label = urllib.parse.unquote(rest[0], errors="strict")
            except UnicodeError:
                return None
            if not label.strip() or len(label) > LABEL_MAX:
                return None
            return kind, owner.lower(), repo, label
        if kind in ("thread-feed", "lite-thread"):
            return kind, owner.lower(), repo, int(rest[0])
        if kind == "feed":
            return kind, owner.lower(), repo, rest[0]
        return kind, owner.lower(), repo
    return None


def origin_of(url):
    parts = urllib.parse.urlsplit(str(url or ""))
    if parts.scheme not in ("http", "https") or not parts.netloc:
        return ""
    return "%s://%s" % (parts.scheme, parts.netloc.lower())


def query_params(scope):
    text = (scope.get("query_string") or b"").decode("latin-1")
    try:
        params = urllib.parse.parse_qs(text, max_num_fields=20)
    except ValueError:
        return {}
    return {key: values[0] for key, values in params.items() if values}


def bounded_int(value, default, low, high):
    try:
        number = int(str(value))
    except (TypeError, ValueError):
        return default
    return min(high, max(low, number))


def esc(value):
    return html.escape(XML_BAD.sub("", "" if value is None else str(value)), quote=True)


def seconds(value):
    return issue_md.flex_seconds(value)


def rfc3339(value):
    moment = seconds(value)
    if moment is None:
        return "1970-01-01T00:00:00Z"
    try:
        return datetime.datetime.fromtimestamp(moment, datetime.timezone.utc).strftime(
            "%Y-%m-%dT%H:%M:%SZ")
    except (OverflowError, OSError, ValueError):
        return "1970-01-01T00:00:00Z"


def short_time(value):
    moment = seconds(value)
    if moment is None:
        return ""
    try:
        return datetime.datetime.fromtimestamp(moment, datetime.timezone.utc).strftime(
            "%Y-%m-%d %H:%MZ")
    except (OverflowError, OSError, ValueError):
        return ""


def http_date(value):
    moment = seconds(value)
    if moment is None:
        return ""
    import email.utils
    return email.utils.formatdate(moment, usegmt=True)


def person(name):
    text = str(name or "").strip() or "unknown"
    forge = issue_md.forge_identity(text)
    if forge:
        return "@" + forge["login"], forge["href"]
    handle = issue_md.person_handle(text)
    return text, ("/@" + urllib.parse.quote(handle, safe="")) if handle \
        and issue_md.USER_NAME_RE.match(handle) else ""


def anchor(event_id, index):
    text = FORGE_PREFIX.sub("", str(event_id or ""))
    text = ANCHOR_SAFE.sub("", text)[:24]
    return "c" + (text or str(index))


def repo_base(owner, repo):
    return "/%s/%s" % (urllib.parse.quote(owner, safe=""), urllib.parse.quote(repo, safe=""))


def label_list(value):
    if not isinstance(value, list):
        return []
    seen, out = set(), []
    for item in value:
        label = str(item or "").strip()[:LABEL_MAX]
        if label and label.lower() not in seen:
            seen.add(label.lower())
            out.append(label)
    return out


# -- reading the repository ----------------------------------------------------

_heads = {}      # (owner, repo) -> (monotonic, commit or "")
_instance = {}   # "doc" -> (monotonic, dict or None)
_listing = {}    # "repos" -> (monotonic, list)
_cache = {}      # key -> (etag, body, last_modified, content_type)
_cache_bytes = [0]


class Source:
    def __init__(self, fastpath, owner, repo):
        self.fastpath = fastpath
        self.owner = owner
        self.repo = repo
        self.commit = ""

    async def read(self, operation, pairs, immutable=False):
        url = self.fastpath.gateway_url(self.owner, self.repo, operation, pairs)
        status, _body, data = await self.fastpath.read(url, immutable)
        if status == 200 and isinstance(data, dict) and data.get("ok") is not False:
            return data
        return None

    async def head(self):
        key = (self.owner, self.repo)
        known = _heads.get(key)
        now = time.monotonic()
        if known is not None and now - known[0] < HEAD_FRESH_SECONDS:
            self.commit = known[1]
            return self.commit
        tree = await self.read("tree", [("path", "")])
        commit = str((tree or {}).get("commit") or "").lower()
        if not re.fullmatch(r"[0-9a-f]{40}|[0-9a-f]{64}", commit):
            commit = ""
        if len(_heads) > 1024:
            _heads.clear()
        _heads[key] = (now, commit)
        self.commit = commit
        return commit

    async def index(self):
        data = await self.read("issue-index", [("ref", self.commit)], True)
        if not isinstance(data, dict) or not isinstance(data.get("issues"), list):
            return None
        return data

    async def blobs(self, paths):
        found = {}

        async def batch(chunk):
            data = await self.read("blobs", [("path", p) for p in chunk]
                                   + [("ref", self.commit)], True)
            for path, blob in ((data or {}).get("blobs") or {}).items():
                if isinstance(blob, dict) and blob.get("encoding") != "base64":
                    found[path] = str(blob.get("content") or blob.get("text") or "")
        await asyncio.gather(*(batch(paths[i:i + BLOB_BATCH])
                               for i in range(0, len(paths), BLOB_BATCH)))
        return found

    async def text(self, path):
        data = await self.read("blob", [("path", path), ("ref", self.commit)], True)
        if not data or data.get("encoding") == "base64":
            return ""
        return str(data.get("content") or "")

    async def threads(self, items, attachments=False):
        paths = {}
        for item in items:
            number = int(item["number"])
            paths[number] = record_path(number, item.get("location", ""))
        wanted = list(paths.values())
        if attachments:
            wanted += [ATTACHMENTS % number for number in paths]
        blobs = await self.blobs(wanted)
        out = {}
        for number, path in paths.items():
            if path not in blobs:
                continue
            files = {}
            if attachments and hasattr(issue_md, "attachment_files"):
                try:
                    files = issue_md.attachment_files(json.loads(
                        blobs.get(ATTACHMENTS % number) or "{}"))
                except ValueError:
                    files = {}
            out[number] = (issue_md.parse_issue(blobs[path], number), files)
        return out


def record_path(number, location):
    folder = "%s/%s" % (ISSUES_DIR, location) if location in ("open", "closed") else ISSUES_DIR
    return "%s/%d/issue-%d.json" % (folder, number, number)


def summaries(index):
    rows = []
    for item in index.get("issues") or []:
        if not isinstance(item, dict) or item.get("deleted"):
            continue
        try:
            number = int(item.get("number") or 0)
        except (TypeError, ValueError):
            continue
        if number <= 0:
            continue
        rows.append({
            "number": number,
            "title": str(item.get("title") or "issue #%d" % number),
            "status": str(item.get("status") or "open").lower(),
            "author": str(item.get("author") or "unknown"),
            "labels": label_list(item.get("labels")),
            "comments": int(item.get("commentCount") or 0),
            "created": seconds(item.get("createdAtMs")) or 0,
            "updated": seconds(item.get("updatedAtMs") or item.get("createdAtMs")) or 0,
            "location": str(item.get("location") or ""),
        })
    rows.sort(key=lambda row: (-row["created"], -row["number"]))
    return rows


def with_label(rows, label):
    wanted = label.strip().lower()
    return [row for row in rows if any(item.lower() == wanted for item in row["labels"])]


def label_counts(rows):
    counts, names = {}, {}
    for row in rows:
        for label in row["labels"]:
            key = label.lower()
            counts[key] = counts.get(key, 0) + 1
            names.setdefault(key, label)
    return [(names[key], count) for key, count in sorted(
        counts.items(), key=lambda pair: (-pair[1], pair[0]))]


def posts(issue):
    events = issue["events"]
    removed = {str(e.get("target")) for e in events if e.get("type") == "delete"
               and e.get("target") and e.get("target") != "self"}
    edits = {}
    for event in events:
        if event.get("type") == "edit" and event.get("target"):
            edits[str(event["target"])] = event
    opened = issue["openEventId"]
    first_edit = edits.get(opened) if opened else None
    out = [{"id": opened or "open", "open": True, "author": issue["author"],
            "ts": issue["createdAt"], "body": str(issue["body"] or ""),
            "updated": (first_edit or {}).get("ts") or issue["createdAt"]}]
    for event in events:
        if event.get("type") != "comment":
            continue
        ident = str(event.get("id") or "")
        if ident in removed:
            continue
        edit = edits.get(ident) if ident else None
        out.append({"id": ident, "open": False,
                    "author": event.get("authorName") or event.get("author") or "unknown",
                    "ts": event.get("ts"), "body": str((edit or event).get("body") or ""),
                    "updated": (edit or {}).get("ts") or event.get("ts")})
    return out


# -- markdown ---------------------------------------------------------------

def context(owner, repo, commit, files=None):
    base = repo_base(owner, repo)
    api = "/api/repo/%s/%s" % (urllib.parse.quote(owner, safe=""),
                               urllib.parse.quote(repo, safe=""))
    if files:
        try:
            return issue_md.Context(base, api, commit, None, False, attachments=files)
        except TypeError:
            pass
    return issue_md.Context(base, api, commit, None, False)


def _image(match):
    attrs = match.group(1)
    found = re.search(r'\ssrc="([^"]*)"', attrs)
    source = html.unescape(found.group(1)) if found else ""
    alt = re.search(r'\salt="([^"]*)"', attrs)
    label = (alt.group(1) if alt else "") or "image"
    if source.startswith("/") and not source.startswith("//") or source.startswith("data:image/"):
        return "<img%s loading=\"lazy\">" % re.sub(r'\sloading="[^"]*"', "", attrs)
    if not source:
        return esc(label)
    return '<a href="%s" rel="nofollow noreferrer">[%s]</a>' % (esc(source), label)


def clean_markup(markup):
    markup = SVG_SRC.sub(' src="', markup)
    markup = STRIP_ATTRS.sub("", markup)
    markup = VIDEO_TAG.sub(lambda m: '<a href="%s">[video]</a>' % m.group(1), markup)
    return IMG_TAG.sub(_image, markup)


def markdown(body, ctx, limit=None):
    text = XML_BAD.sub("", str(body or ""))
    cut = limit is not None and len(text) > limit
    if cut:
        text = text[:limit]
    if not text.strip():
        return "", cut
    return clean_markup(issue_md.render_markdown(text[:issue_md.MAX_BODY_CHARS], ctx)), cut


def absolute(markup, origin):
    return ROOT_URL.sub(lambda m: '%s="%s/' % (m.group(1), origin), markup)


# -- Atom ---------------------------------------------------------------------

def tag_id(origin, *parts):
    host = urllib.parse.urlsplit(origin).hostname or "localhost"
    return "tag:%s,%s:%s" % (host, TAG_DATE, "/".join(
        urllib.parse.quote(str(part), safe="-._~") for part in parts))


def thread_tag(origin, owner, repo, number):
    return tag_id(origin, owner, repo, "issues", number)


def atom_author(name, origin):
    shown, href = person(name)
    uri = href if href.startswith("https://") else (origin + href if href else "")
    return "<author><name>%s</name>%s</author>" % (
        esc(shown), "<uri>%s</uri>" % esc(uri) if uri else "")


def atom_entry(origin, owner, repo, row, post, ctx, total=None):
    base = origin + repo_base(owner, repo)
    number = row["number"]
    body, cut = markdown(post["body"], ctx, FEED_BODY_CHARS)
    thread_href = "%s/issues/%d" % (base, number)
    if cut:
        body += '<p><a href="%s">Continue reading</a></p>' % esc(thread_href)
    body = absolute(body, origin)
    parts = ["<entry>"]
    if post["open"]:
        parts.append("<id>%s</id>" % thread_tag(origin, owner, repo, number))
        parts.append("<title>%s</title>" % esc("#%d %s" % (number, row["title"])))
        parts.append('<link rel="alternate" type="text/html" href="%s"/>' % esc(thread_href))
        replies = row["comments"] if total is None else total
        parts.append('<link rel="replies" type="application/atom+xml" href="%s.atom" '
                     'thr:count="%d"/>' % (esc(thread_href), replies))
        parts.append("<thr:total>%d</thr:total>" % replies)
    else:
        mark = anchor(post["id"], 0)
        parts.append("<id>%s</id>" % tag_id(origin, owner, repo, "issues", number,
                                             "comment", post["id"]))
        shown, _href = person(post["author"])
        parts.append("<title>%s</title>" % esc("Re: #%d %s (%s)" % (
            number, row["title"], shown)))
        parts.append('<link rel="alternate" type="text/html" href="%s/lite/issues/%d#%s"/>'
                     % (esc(base), number, mark))
        parts.append('<thr:in-reply-to ref="%s" type="text/html" href="%s"/>' % (
            thread_tag(origin, owner, repo, number), esc(thread_href)))
    parts.append("<published>%s</published>" % rfc3339(post["ts"]))
    parts.append("<updated>%s</updated>" % rfc3339(post["updated"] or post["ts"]))
    parts.append(atom_author(post["author"], origin))
    if post["open"]:
        parts.extend('<category term="%s"/>' % esc(label) for label in row["labels"])
    parts.append('<content type="html">%s</content>' % esc(body))
    parts.append("</entry>")
    return "".join(parts)


def atom_feed(origin, feed_id, title, self_href, alt_href, entries, updated):
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<feed xmlns="http://www.w3.org/2005/Atom" '
            'xmlns:thr="http://purl.org/syndication/thread/1.0">'
            '<id>%s</id><title>%s</title><updated>%s</updated>'
            '<link rel="self" type="application/atom+xml" href="%s"/>'
            '<link rel="alternate" type="text/html" href="%s"/>%s</feed>\n' % (
                feed_id, esc(title), rfc3339(updated), esc(self_href), esc(alt_href),
                "".join(entries)))


async def feed_threads(source, rows, origin, limit, self_path, title, feed_name):
    owner, repo = source.owner, source.repo
    chosen = rows[:limit]
    records = await source.threads(chosen, attachments=True)
    entries, newest = [], 0
    for row in chosen:
        found = records.get(row["number"])
        if not found:
            continue
        issue, files = found
        if issue["deleted"]:
            continue
        first = posts(issue)[0]
        ctx = context(owner, repo, source.commit, files)
        entries.append(atom_entry(origin, owner, repo, row, first, ctx,
                                  issue_md.comment_count(issue["events"])))
        newest = max(newest, seconds(first["updated"]) or 0, seconds(first["ts"]) or 0)
    base = origin + repo_base(owner, repo)
    body = atom_feed(origin, tag_id(origin, owner, repo, *feed_name), title,
                     origin + self_path, base + "/issues", entries, newest or row_time(rows))
    return body, newest


def row_time(rows):
    return max((row["updated"] for row in rows), default=0)


async def feed_activity(source, rows, origin, limit, self_path):
    owner, repo = source.owner, source.repo
    active = sorted(rows, key=lambda row: (-row["updated"], -row["number"]))[:limit]
    records = await source.threads(active, attachments=True)
    items = []
    by_number = {row["number"]: row for row in active}
    for number, (issue, files) in records.items():
        if issue["deleted"]:
            continue
        thread = posts(issue)
        for post in thread:
            items.append((seconds(post["ts"]) or 0, number, post, files, len(thread) - 1))
    items.sort(key=lambda item: (-item[0], -item[1]))
    entries, newest = [], 0
    for stamp, number, post, files, total in items[:limit]:
        ctx = context(owner, repo, source.commit, files)
        entries.append(atom_entry(origin, owner, repo, by_number[number], post, ctx, total))
        newest = max(newest, stamp)
    base = origin + repo_base(owner, repo)
    body = atom_feed(origin, tag_id(origin, owner, repo, "activity"),
                     "%s/%s: new threads and comments" % (owner, repo),
                     origin + self_path, base + "/issues", entries, newest or row_time(rows))
    return body, newest


async def feed_thread(source, row, origin, limit, self_path):
    owner, repo = source.owner, source.repo
    records = await source.threads([row], attachments=True)
    found = records.get(row["number"])
    if not found or found[0]["deleted"]:
        return None, 0
    issue, files = found
    thread = posts(issue)
    total = len(thread) - 1
    ctx = context(owner, repo, source.commit, files)
    ordered = sorted(thread, key=lambda post: -(seconds(post["ts"]) or 0))[:limit]
    entries = [atom_entry(origin, owner, repo, row, post, ctx, total) for post in ordered]
    newest = max((seconds(post["updated"]) or seconds(post["ts"]) or 0 for post in thread),
                 default=0)
    base = origin + repo_base(owner, repo)
    body = atom_feed(origin, tag_id(origin, owner, repo, "issues", row["number"], "feed"),
                     "#%d %s" % (row["number"], row["title"]), origin + self_path,
                     "%s/issues/%d" % (base, row["number"]), entries, newest)
    return body, newest


# -- lite pages ---------------------------------------------------------------

def page(title, body, head_links=""):
    return ('<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" '
            'content="width=device-width,initial-scale=1"><meta name="robots" '
            'content="noarchive"><title>%s</title>%s%s<body><!--email_off-->%s<!--/email_off--></body></html>\n' % (
                esc(title), head_links, STYLE, body))


def feed_link(href, title):
    return '<link rel="alternate" type="application/atom+xml" title="%s" href="%s">' % (
        esc(title), esc(href))


def head_links(owner, repo, number=0):
    base = repo_base(owner, repo)
    links = [feed_link(base + "/issues.atom", "%s/%s: new threads" % (owner, repo)),
             feed_link(base + "/activity.atom", "%s/%s: threads and comments" % (owner, repo))]
    if number:
        links.insert(0, feed_link("%s/issues/%d.atom" % (base, number),
                                  "%s/%s #%d" % (owner, repo, number)))
    return "".join(links)


def noscript(owner, repo, number=0):
    base = repo_base(owner, repo)
    lite = "%s/lite/issues/%d" % (base, number) if number else base + "/lite"
    feed = "%s/issues/%d.atom" % (base, number) if number else base + "/issues.atom"
    return ('<noscript><p style="margin:8px 16px"><a href="%s">Lite view (no JavaScript)'
            '</a> &middot; <a href="%s">Atom feed</a></p></noscript>' % (esc(lite), esc(feed)))


def label_href(base, label):
    return "%s/lite?label=%s" % (base, urllib.parse.quote(label, safe=""))


def labels_html(base, labels):
    return ", ".join('<a href="%s">%s</a>' % (esc(label_href(base, label)), esc(label))
                     for label in labels)


def row_html(base, row, lite=True):
    meta = ["#%d" % row["number"], esc(person(row["author"])[0]), short_time(row["created"])[:10]]
    if row["comments"]:
        meta.append("%d comment%s" % (row["comments"], "" if row["comments"] == 1 else "s"))
    if row["status"] == "closed":
        meta.append("closed")
    labels = labels_html(base, row["labels"][:6])
    return '<li><a href="%s%s/issues/%d" dir="auto">%s</a><br><span class="m">%s%s</span>' % (
        esc(base), "/lite" if lite else "", row["number"], esc(row["title"]), " &middot; ".join(meta),
        " &middot; " + labels if labels else "")


def lite_list(owner, repo, rows, params):
    base = repo_base(owner, repo)
    label = str(params.get("label") or "").strip()[:LABEL_MAX]
    active = params.get("sort") == "active"
    everything = rows
    if label:
        rows = with_label(rows, label)
    if active:
        rows = sorted(rows, key=lambda row: (-row["updated"], -row["number"]))
    pages = max(1, (len(rows) + PAGE_SIZE - 1) // PAGE_SIZE)
    number = bounded_int(params.get("page"), 1, 1, pages)
    shown = rows[(number - 1) * PAGE_SIZE:number * PAGE_SIZE]

    def link(page_number=None, sort=None, chosen=None):
        query = []
        name = label if chosen is None else chosen
        if name:
            query.append(("label", name))
        if sort if sort is not None else active:
            query.append(("sort", "active"))
        if page_number and page_number > 1:
            query.append(("page", str(page_number)))
        return base + "/lite" + ("?" + urllib.parse.urlencode(query) if query else "")
    feed = (base + "/labels/%s.atom" % urllib.parse.quote(label, safe="")) if label \
        else base + "/issues.atom"
    head = ['<nav><a href="/lite">Lite</a> / <b>%s/%s</b></nav>' % (esc(owner), esc(repo)),
            "<h1>%s%s</h1>" % (esc("%s/%s" % (owner, repo)),
                               (": " + esc(label)) if label else ""),
            '<p class="m">%d thread%s &middot; %s &middot; <a href="%s">Feed</a> &middot; '
            '<a href="%s/activity.atom">All activity</a> &middot; <a href="%s/issues">'
            'Full view</a></p>' % (
                len(rows), "" if len(rows) == 1 else "s",
                '<a href="%s">Newest</a>' % esc(link(sort=False)) if active
                else '<a href="%s">Recently active</a>' % esc(link(sort=True)),
                esc(feed), esc(base), esc(base))]
    if label:
        head.append('<p class="m"><a href="%s">All threads</a></p>' % esc(link(chosen="")))
    else:
        chips = label_counts(everything)[:LIST_LABELS]
        if chips:
            head.append('<p class="m">Labels: %s</p>' % ", ".join(
                '<a href="%s">%s</a>' % (esc(label_href(base, name)), esc(name))
                for name, _count in chips))
    items = "".join(row_html(base, row) for row in shown) or "<li>No threads.</li>"
    nav = []
    if number > 1:
        nav.append('<a href="%s" rel="prev">Newer</a>' % esc(link(number - 1)))
    nav.append("Page %d of %d" % (number, pages))
    if number < pages:
        nav.append('<a href="%s" rel="next">Older</a>' % esc(link(number + 1)))
    body = "".join(head) + '<ul class="t">%s</ul><nav>%s</nav>' % (items, " &middot; ".join(nav))
    title = "%s/%s%s" % (owner, repo, (" - " + label) if label else "")
    links = feed_link(feed, title) + feed_link(base + "/activity.atom", "%s/%s activity" % (
        owner, repo))
    return page(title, body, links)


def neighbours(rows, number):
    numbers = sorted(row["number"] for row in rows)
    lower = [n for n in numbers if n < number]
    higher = [n for n in numbers if n > number]
    return (lower[-1] if lower else 0), (higher[0] if higher else 0)


def lite_thread(owner, repo, row, issue, files, commit, rows):
    base = repo_base(owner, repo)
    number = row["number"]
    ctx = context(owner, repo, commit, files)
    parts = ['<nav><a href="%s/lite">%s/%s</a></nav>' % (esc(base), esc(owner), esc(repo)),
             '<h1 dir="auto">%s <span class="m">#%d</span></h1>' % (esc(row["title"]), number)]
    meta = [issue["status"].lower() if issue["status"] else "open"]
    if row["labels"]:
        meta.append(labels_html(base, row["labels"]))
    parts.append('<p class="m">%s</p>' % " &middot; ".join(meta))
    for index, post in enumerate(posts(issue)):
        body, _cut = markdown(post["body"], ctx)
        parts.append('<article id="%s"><p><b>%s</b> <time>%s</time></p><div dir="auto">%s'
                     '</div></article>' % (
                         "c0" if post["open"] else anchor(post["id"], index),
                         esc(person(post["author"])[0]), short_time(post["ts"]), body))
    older, newer = neighbours(rows, number)
    nav = ['<a href="%s/lite">All threads</a>' % esc(base)]
    if older:
        nav.append('<a href="%s/lite/issues/%d" rel="prev">#%d</a>' % (esc(base), older, older))
    if newer:
        nav.append('<a href="%s/lite/issues/%d" rel="next">#%d</a>' % (esc(base), newer, newer))
    nav.append('<a href="%s/issues/%d">Full view</a>' % (esc(base), number))
    nav.append('<a href="%s/issues/%d.atom">Feed</a>' % (esc(base), number))
    parts.append("<hr><nav>%s</nav>" % " &middot; ".join(nav))
    return page("#%d %s" % (number, row["title"]), "".join(parts), feed_link(
        "%s/issues/%d.atom" % (base, number), "#%d" % number))


def lite_missing(message):
    return page("Not found", '<p>%s</p><p><a href="/lite">Lite</a></p>' % esc(message))


def lite_home(name, repos, home):
    items = []
    for owner, repo, about in repos:
        base = repo_base(owner, repo)
        items.append('<li><a href="%s/lite">%s/%s</a> <span class="m">&middot; '
                     '<a href="%s/issues.atom">feed</a></span>%s' % (
                         esc(base), esc(owner), esc(repo), esc(base),
                         '<br><span class="m" dir="auto">%s</span>' % esc(about) if about else ""))
    body = ("<h1>%s</h1><p class=\"m\">Plain pages for slow or filtered connections: no "
            "JavaScript, no images from other sites. <a href=\"/\">Home</a></p>"
            '<ul class="t" id="feeds">%s</ul>' % (esc(name), "".join(items)
                                                  or "<li>No public repositories.</li>"))
    links = ""
    if home:
        links = head_links(*home)
    return page("%s - lite" % name, body, links)


# -- forum home ---------------------------------------------------------------

HOME_STYLE = (
    "<style>header{display:flex;flex-wrap:wrap;gap:.5em 1em;align-items:center;"
    "border-bottom:1px solid var(--r);padding:.5em 0}header b{font-size:1.1em;"
    "margin-inline-end:auto}header img{width:28px;height:28px;vertical-align:middle;"
    "margin-inline-end:.4em}section{margin:1em 0}.chips a{display:inline-block;"
    "margin:.15em .2em;padding:0 .5em;border:1px solid var(--r);border-radius:1em;"
    "text-decoration:none;font-size:.85em}</style>")


def readme_sections(markup):
    chunks = [chunk for chunk in re.split(r"(?=<h[1-3][\s>])", markup) if chunk.strip()]
    return "".join('<section dir="auto">%s</section>' % chunk for chunk in chunks)


def home_page(doc, owner, repo, rows, readme, quota):
    base = repo_base(owner, repo)
    name = str(doc.get("name") or "%s/%s" % (owner, repo))
    logo = str(doc.get("logo") or "")
    logo_html = '<img src="%s" alt="">' % esc(logo) if logo.startswith("/") \
        and not logo.startswith("//") else ""
    header = ('<header><b><a href="/" style="text-decoration:none;color:inherit">%s%s</a>'
              '</b><a href="%s/issues">Threads</a><a href="%s/lite">Lite</a>'
              '<a href="%s/activity.atom">Feed</a><a href="/dashboard">Dashboard</a>'
              '<a href="/login">Sign in</a></header>' % (
                  logo_html, esc(name), esc(base), esc(base), esc(base)))
    latest = "".join(row_html(base, row, False) for row in rows[:HOME_THREADS])
    chips = "".join('<a href="%s">%s <span class="m">%d</span></a>' % (
        esc(label_href(base, label)), esc(label), count)
        for label, count in label_counts(rows)[:HOME_LABELS])
    parts = [header]
    if readme:
        parts.append('<section data-forum-readme>%s</section>' % readme_sections(readme))
    parts.append('<section><h2>Latest threads</h2><ul class="t">%s</ul><p class="m">'
                 '<a href="%s/issues">All threads</a> &middot; <a href="%s/lite">Lite view</a>'
                 ' &middot; <a href="%s/issues.atom">New threads feed</a> &middot; '
                 '<a href="%s/activity.atom">Threads and comments feed</a></p></section>' % (
                     latest or "<li>No threads yet.</li>", esc(base), esc(base), esc(base),
                     esc(base)))
    if chips:
        parts.append('<section class="chips"><h2>Labels</h2>%s</section>' % chips)
    parts.append('<section data-forum-mirrors>%s</section>' % quota_line(base, quota))
    return page(name, "".join(parts), head_links(owner, repo) + HOME_STYLE)


def quota_line(base, quota):
    if not isinstance(quota, dict) or not quota.get("ok"):
        return ""
    mirrors = quota.get("mirrors") if isinstance(quota.get("mirrors"), list) else []
    healthy = quota.get("healthy")
    goal = quota.get("target")
    if not isinstance(healthy, int) or isinstance(healthy, bool):
        healthy = sum(1 for item in mirrors if isinstance(item, dict)
                      and item.get("status") == "green")
    text = "%d healthy mirror%s" % (healthy, "" if healthy == 1 else "s")
    if isinstance(goal, int) and not isinstance(goal, bool) and goal > 0:
        text += " of %d wanted" % goal
    clone = str(quota.get("cloneUrl") or "")
    return '<p class="m">%s &middot; <a href="%s/mirrors">Mirrors</a>%s</p>' % (
        esc(text), esc(base), " &middot; clone <code>%s</code>" % esc(clone) if clone else "")


# -- Worker reads ---------------------------------------------------------------

async def worker_json(worker_fetch, origin, path, budget=WORKER_BUDGET_SECONDS):
    if worker_fetch is None or not origin:
        return None
    from . import js as _js
    headers = _js.Headers({"host": urllib.parse.urlsplit(origin).netloc,
                           "user-agent": "forkmesh-host-read-surfaces",
                           "cf-connecting-ip": "127.0.0.1",
                           "accept": "application/json"})

    async def run():
        response = await worker_fetch(_js.Request(origin + path, "GET", headers))
        if int(response.status) != 200:
            return None
        data = json.loads(await response._read())
        return data if isinstance(data, dict) else None
    try:
        return await asyncio.wait_for(run(), budget)
    except Exception:
        return None


async def instance_doc(worker_fetch, origin):
    known = _instance.get(origin)
    now = time.monotonic()
    if known is not None and now - known[0] < INSTANCE_FRESH_SECONDS:
        return known[1]
    doc = await worker_json(worker_fetch, origin, "/api/instance")
    if doc is None and known is not None:
        return known[1]
    if len(_instance) > 64:
        _instance.clear()
    _instance[origin] = (now, doc)
    return doc


def home_repository(doc):
    home = (doc or {}).get("home") if isinstance(doc, dict) else None
    value = str((home or {}).get("repository") or "") if isinstance(home, dict) else ""
    match = re.fullmatch(r"([a-z0-9][a-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})", value)
    if not match or match.group(2) in (".", "..") or match.group(2).endswith(".git"):
        return None
    return match.group(1), match.group(2)


async def public_repositories(worker_fetch, origin):
    known = _listing.get(origin)
    now = time.monotonic()
    if known is not None and now - known[0] < INSTANCE_FRESH_SECONDS:
        return known[1]
    data = await worker_json(worker_fetch, origin, "/api/repositories")
    repos = []
    for item in (data or {}).get("repositories") or []:
        if not isinstance(item, dict) or item.get("visibility", "public") != "public":
            continue
        owner, repo = str(item.get("owner") or "").lower(), str(item.get("name") or "")
        if re.fullmatch(OWNER, owner) and re.fullmatch(REPO, repo):
            repos.append((owner, repo, str(item.get("description") or "")[:200]))
    if data is None and known is not None:
        return known[1]
    if len(_listing) > 64:
        _listing.clear()
    _listing[origin] = (now, repos)
    return repos


# -- caching and the ASGI answer ---------------------------------------------------

def remember(key, value):
    old = _cache.pop(key, None)
    if old is not None:
        _cache_bytes[0] -= len(old[1])
    if len(value[1]) > CACHE_BYTES // 4:
        return
    while _cache and _cache_bytes[0] + len(value[1]) > CACHE_BYTES:
        _first, dropped = next(iter(_cache.items()))
        _cache.pop(_first)
        _cache_bytes[0] -= len(dropped[1])
    _cache[key] = value
    _cache_bytes[0] += len(value[1])


def etag_for(key):
    return '"fm-rs-%s"' % hashlib.sha256(repr(key).encode("utf-8")).hexdigest()[:24]


def not_modified(scope, etag):
    for name, value in scope.get("headers") or []:
        if name == b"if-none-match":
            tags = [part.strip() for part in value.decode("latin-1").split(",")]
            return etag in tags or "*" in tags
    return False


async def send_answer(scope, send, status, body, content_type, etag="", modified="",
                      cache=CACHE_CONTROL):
    headers = [(b"content-type", content_type.encode()),
               (b"cache-control", cache.encode()),
               (b"x-content-type-options", b"nosniff"),
               (b"referrer-policy", b"same-origin"),
               (b"vary", b"accept-encoding")]
    if content_type.startswith("text/html"):
        headers.append((b"content-security-policy", CSP.encode()))
    if etag:
        headers.append((b"etag", etag.encode()))
    if modified:
        headers.append((b"last-modified", modified.encode()))
    if etag and status == 200 and not_modified(scope, etag):
        await send({"type": "http.response.start", "status": 304, "headers": headers})
        await send({"type": "http.response.body", "body": b""})
        return
    headers.append((b"content-length", str(len(body)).encode()))
    await send({"type": "http.response.start", "status": status, "headers": headers})
    head = scope.get("method", "GET").upper() == "HEAD"
    await send({"type": "http.response.body", "body": b"" if head else body})


PLUGIN = "read-surfaces"


def plugin_on(doc, plugin=PLUGIN):
    for item in (doc or {}).get("plugins") or []:
        if isinstance(item, dict) and item.get("id") == plugin:
            return item.get("enabled") is True
    return False


async def handle(fastpath, worker_fetch, scope, send, origin):
    found = target(scope)
    if found is None or fastpath is None or not origin:
        return False
    if found[0] == "home" and scope.get("query_string"):
        return False
    if not plugin_on(await instance_doc(worker_fetch, origin)):
        return False
    params = query_params(scope)
    if found[0] == "home":
        doc = await instance_doc(worker_fetch, origin)
        home = home_repository(doc)
        if home is None:
            return False
        return await _answer(fastpath, worker_fetch, scope, send, origin,
                             ("home",) + home, params, doc)
    if found[0] == "lite-home":
        doc = await instance_doc(worker_fetch, origin) or {}
        repos = await public_repositories(worker_fetch, origin)
        home = home_repository(doc)
        if home is not None:
            repos = [item for item in repos if item[:2] == home] + \
                [item for item in repos if item[:2] != home]
            if not any(item[:2] == home for item in repos):
                repos.insert(0, home + ("",))
        name = str(doc.get("name") or "ForkMesh")
        body = lite_home(name, repos, home).encode("utf-8")
        await send_answer(scope, send, 200, body, HTML_TYPE, etag_for(("lite-home", body)))
        return True
    return await _answer(fastpath, worker_fetch, scope, send, origin, found, params, None)


async def _answer(fastpath, worker_fetch, scope, send, origin, found, params, doc):
    kind, owner, repo = found[0], found[1], found[2]
    source = Source(fastpath, owner, repo)
    if not await source.head():
        return False
    limit = bounded_int(params.get("limit"), FEED_LIMIT, 1, FEED_LIMIT_MAX)
    label = str(params.get("label") or "").strip()[:LABEL_MAX]
    if kind == "label-feed":
        label = found[3]
    key_params = (limit, label) if kind in ("feed", "label-feed", "thread-feed") else \
        tuple(sorted((k, v) for k, v in params.items() if k in ("page", "label", "sort")))
    key = (kind, owner, repo, found[3:], key_params, source.commit, origin,
           (json.dumps(doc, sort_keys=True), int(time.time() // HOME_SECONDS))
           if kind == "home" else "")
    etag = etag_for(key)
    if etag and not_modified(scope, etag):
        await send_answer(scope, send, 200, b"", ATOM_TYPE if "feed" in kind else HTML_TYPE,
                          etag)
        return True
    kept = _cache.get(key)
    if kept is not None:
        await send_answer(scope, send, 200, kept[1], kept[3], kept[0], kept[2])
        return True
    index = await source.index()
    if index is None:
        return False
    rows = summaries(index)
    status, content_type, newest = 200, ATOM_TYPE, 0
    if kind in ("feed", "label-feed"):
        path = scope.get("path") or ""
        self_path = path + ("?" + scope["query_string"].decode("latin-1")
                            if scope.get("query_string") else "")
        if label:
            chosen = with_label(rows, label)
            title = "%s/%s: %s" % (owner, repo, label)
            body, newest = await feed_threads(source, chosen, origin, limit, self_path, title,
                                              ("labels", label))
        elif found[3] == "activity":
            body, newest = await feed_activity(source, rows, origin, limit, self_path)
        else:
            body, newest = await feed_threads(source, rows, origin, limit, self_path,
                                              "%s/%s: new threads" % (owner, repo), ("issues",))
    elif kind == "thread-feed":
        row = next((row for row in rows if row["number"] == found[3]), None)
        body = None
        if row is not None:
            body, newest = await feed_thread(source, row, origin, limit, scope.get("path") or "")
        if body is None:
            await send_answer(scope, send, 404, b"no such thread\n",
                              "text/plain; charset=utf-8", cache="no-cache")
            return True
    elif kind == "lite":
        content_type = HTML_TYPE
        body = lite_list(owner, repo, rows, params)
        newest = row_time(rows)
    elif kind == "lite-thread":
        content_type = HTML_TYPE
        row = next((row for row in rows if row["number"] == found[3]), None)
        records = await source.threads([row], attachments=True) if row else {}
        record = records.get(found[3]) if row else None
        if record is None or record[0]["deleted"]:
            await send_answer(scope, send, 404, lite_missing(
                "There is no thread #%d here." % found[3]).encode("utf-8"), HTML_TYPE,
                cache="no-cache")
            return True
        body = lite_thread(owner, repo, row, record[0], record[1], source.commit, rows)
        newest = row["updated"]
    else:
        content_type = HTML_TYPE
        readme = await home_readme(source)
        quota = await worker_json(worker_fetch, origin, "/api/repo/%s/%s/mirror-quota" % (
            urllib.parse.quote(owner, safe=""), urllib.parse.quote(repo, safe="")),
            QUOTA_BUDGET_SECONDS)
        recent = sorted(rows, key=lambda row: (-row["updated"], -row["number"]))
        body = home_page(doc or {}, owner, repo, recent, readme, quota)
        newest = row_time(rows)
    data = body.encode("utf-8")
    modified = http_date(newest) if newest else ""
    remember(key, (etag, data, modified, content_type))
    await send_answer(scope, send, status, data, content_type, etag, modified)
    return True


README_NAMES = ("README.md", "readme.md", "Readme.md", "README", "README.txt")


async def home_readme(source):
    tree = await source.read("tree", [("path", ""), ("ref", source.commit)], True)
    names = {str(entry.get("name") or "") for entry in (tree or {}).get("entries") or []
             if isinstance(entry, dict) and entry.get("type") == "blob"}
    name = next((candidate for candidate in README_NAMES if candidate in names), "")
    if not name:
        return ""
    text = await source.text(name)
    body, _cut = markdown(text, context(source.owner, source.repo, source.commit))
    return body
