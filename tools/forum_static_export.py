#!/usr/bin/env python3
from __future__ import annotations

import argparse
import datetime
import gzip
import hashlib
import html
import io
import json
import os
import re
import subprocess
import sys
import tarfile
import urllib.parse
from pathlib import Path
from typing import Any, Iterable

GENERATOR = "forkmesh-forum-static-export/1"
MANIFEST_SCHEMA = "forkmesh-static-export-v1"
ISSUES_REL = ".forkmesh/issues"
ATTACHMENTS_REL = ".forkmesh/attachments"
ATTACHMENT_INDEX_REL = ATTACHMENTS_REL + "/index.json"
CONTRIBUTORS_REL = ".forkmesh/contributors"
README_NAMES = ("README.md", "readme.md", "README.markdown", "README")
STORED_RE = re.compile(r"^\.forkmesh/attachments/([0-9a-f]{64}\.[a-z0-9]{1,8})$")
AVATAR_RE = re.compile(r"^[a-z0-9-]{1,20}-[a-z0-9._-]{1,40}\.png$")
FORGE_NAME_RE = re.compile(r"^([a-z]{2,20}):([A-Za-z0-9](?:[A-Za-z0-9._-]{0,38}[A-Za-z0-9])?)$")
URL_RE = re.compile(r"https?://[^\s<>\"'()\[\]`]+")
REF_RE = re.compile(r"#([1-9][0-9]{0,8})\b")
IMAGE_TYPES = {"png", "jpg", "jpeg", "gif", "webp", "avif", "svg", "bmp", "ico"}
TAG_RE = re.compile(r"<(/?)(a|img)\b([^>]*)>", re.I)
ATTR_RE = re.compile(r'\s(href|src|alt)="([^"]*)"')
DROP_ATTR_RE = re.compile(
    r'\s(?:class|target|decoding|referrerpolicy|data-[a-z0-9-]+|aria-label)(?:="[^"]*")?', re.I)
HEADING_ID_RE = re.compile(r'\sdata-repo-markdown-heading="([^"]*)"')
OPEN_TAG_RE = re.compile(r"<[A-Za-z][A-Za-z0-9]*\b[^>]*>")
SVG_RE = re.compile(r"<svg\b[\s\S]*?</svg>", re.I)
STATE_FILE = "manifest.json"
DEFAULT_LATEST = 30
DEFAULT_FEED_SIZE = 100
DEFAULT_MAX_ATTACHMENT = 20 * 1024 * 1024
EXPORT_VERSION = "3"

CSS = """:root{color-scheme:light dark;--fg:#1f2328;--mu:#59636e;--bg:#fff;--bd:#d1d9e0;--ln:#0969da;--cd:#f6f8fa}
@media (prefers-color-scheme:dark){:root{--fg:#e6edf3;--mu:#9198a1;--bg:#0d1117;--bd:#30363d;--ln:#4493f8;--cd:#151b23}}
*{box-sizing:border-box}
body{margin:0 auto;max-width:52rem;padding:0 1rem 3rem;font:15px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif;color:var(--fg);background:var(--bg);overflow-wrap:anywhere}
a{color:var(--ln)}
header,footer{padding:.6rem 0;color:var(--mu);font-size:13px}
header{border-bottom:1px solid var(--bd)}
footer{border-top:1px solid var(--bd);margin-top:2rem}
header b a{color:var(--fg);text-decoration:none}
h1{font-size:1.45rem;line-height:1.3;margin:1rem 0 .3rem}
h2{font-size:1.1rem;margin:1.6rem 0 .4rem}
.m,.by{color:var(--mu);font-size:13px}
ol.t,ul.t{padding-left:0;list-style:none}
.t li{padding:.3rem 0;border-bottom:1px solid var(--bd)}
.t .m{display:block}
.post{border-top:1px solid var(--bd);padding:.6rem 0}
.by img{vertical-align:middle;border-radius:50%;margin-right:.3rem}
.md img,.md video{max-width:100%;height:auto}
pre{overflow:auto;background:var(--cd);padding:.6rem;border-radius:6px;font-size:13px}
code{font-size:.9em}
blockquote{margin:.5rem 0;padding-left:.8rem;border-left:3px solid var(--bd);color:var(--mu)}
table{border-collapse:collapse;display:block;overflow-x:auto}
td,th{border:1px solid var(--bd);padding:.2rem .5rem}
.lb{display:inline-block;border:1px solid var(--bd);border-radius:1rem;padding:0 .5rem;margin:0 .2rem .2rem 0;font-size:12px}
.x{color:#8250df}
"""


class ExportError(RuntimeError):
    pass


def _host_renderer():
    here = Path(__file__).resolve().parent
    host = here.parent / "app" / "host"
    if host.is_dir() and str(host) not in sys.path:
        sys.path.append(str(host))
    try:
        from forkmesh_host import repo_ssr_issue
    except Exception:
        return None
    return repo_ssr_issue


def esc(value: Any) -> str:
    return html.escape("" if value is None else str(value), quote=True)


class FallbackMarkdown:
    INLINE_CODE = re.compile(r"`([^`]+)`")
    IMAGE = re.compile(r"!\[([^\]]*)\]\(([^)\s]+)\)")
    LINK = re.compile(r"\[([^\]]+)\]\(([^)\s]+)\)")
    BARE = re.compile(r"(^|[\s(])(https?://[^\s<>()\x01]+[^\s<>().,;:!?\x01])")

    def __init__(self, ctx: "StaticContext") -> None:
        self.ctx = ctx

    @staticmethod
    def safe(url: str) -> str:
        url = html.unescape(url).strip()
        if re.match(r"^(https?:|mailto:|#)", url, re.I):
            return url
        if re.match(r"^[a-z][a-z0-9+.-]*:", url, re.I):
            return ""
        return url

    def inline(self, text: str) -> str:
        parked: list[str] = []

        def park(markup: str) -> str:
            parked.append(markup)
            return "\x01%d\x01" % (len(parked) - 1)

        text = esc(text)
        text = self.INLINE_CODE.sub(lambda m: park("<code>%s</code>" % m.group(1)), text)

        def image(match: re.Match) -> str:
            url = self.safe(match.group(2))
            if url and not re.match(r"^[a-z]+:|^#", url, re.I):
                url = self.ctx.raw_url(url)
            return park('<img src="%s" alt="%s">' % (esc(url), match.group(1))) if url else match.group(1)

        def link(match: re.Match) -> str:
            url = self.safe(match.group(2))
            if not url:
                return match.group(1)
            if not re.match(r"^[a-z]+:|^#", url, re.I):
                url = self.ctx.path_url("blob", url)
            return park('<a href="%s">%s</a>' % (esc(url), match.group(1)))

        text = self.IMAGE.sub(image, text)
        text = self.LINK.sub(link, text)
        text = self.BARE.sub(lambda m: m.group(1) + park(
            '<a href="%s">%s</a>' % (m.group(2), m.group(2))), text)
        text = re.sub(r"(^|[\s(])#([1-9][0-9]{0,8})\b", lambda m: m.group(1) + park(
            '<a href="%s">#%s</a>' % (esc(self.ctx.record_url("issues", m.group(2))), m.group(2))), text)
        text = re.sub(r"(\*\*|__)(.+?)\1", r"<strong>\2</strong>", text)
        text = re.sub(r"(^|[^\w*])\*([^*\n]+)\*", r"\1<em>\2</em>", text)
        return re.sub("\x01(\\d+)\x01", lambda m: parked[int(m.group(1))], text)

    def render(self, raw: str) -> str:
        lines = re.sub(r"\r\n?", "\n", raw or "").split("\n")
        out: list[str] = []
        para: list[str] = []
        items: list[str] = []

        def flush() -> None:
            if para:
                out.append("<p>%s</p>" % self.inline(" ".join(para)))
                para.clear()
            if items:
                out.append("<ul>%s</ul>" % "".join("<li>%s</li>" % self.inline(i) for i in items))
                items.clear()

        i = 0
        while i < len(lines):
            line = lines[i]
            fence = re.match(r"^\s*(```|~~~)", line)
            if fence:
                flush()
                code = []
                i += 1
                while i < len(lines) and not lines[i].strip().startswith(fence.group(1)):
                    code.append(lines[i])
                    i += 1
                out.append("<pre><code>%s</code></pre>" % esc("\n".join(code)))
                i += 1
                continue
            heading = re.match(r"^(#{1,6})\s+(.*)$", line)
            item = re.match(r"^\s*(?:[-*+]|\d+[.)])\s+(.*)$", line)
            if heading:
                flush()
                level = len(heading.group(1))
                out.append("<h%d>%s</h%d>" % (level, self.inline(heading.group(2)), level))
            elif re.match(r"^>\s?", line):
                flush()
                out.append("<blockquote>%s</blockquote>" % self.inline(re.sub(r"^>\s?", "", line)))
            elif item:
                if para:
                    flush()
                items.append(item.group(1))
            elif not line.strip():
                flush()
            else:
                if items:
                    flush()
                para.append(line.strip())
            i += 1
        flush()
        return "".join(out)


class StaticContext:
    def __init__(self, prefix: str, numbers: frozenset[int], source_url: str) -> None:
        self.prefix = prefix
        self.numbers = numbers
        self.source_url = source_url.rstrip("/")
        self.link_base = ""
        self.api_base = ""
        self.ref = "HEAD"
        self.issue_tasks = None
        self.figures = False
        self.origin = ""

    def record_url(self, kind: str, number: Any) -> str:
        number = int(number)
        if kind == "issues" and number in self.numbers:
            return "%sissues/%d.html" % (self.prefix, number)
        if self.source_url:
            return "%s/%s/%d" % (self.source_url, "pull" if kind == "pulls" else "issues", number)
        return "fmnolink:"

    def path_url(self, kind: str = "tree", path: str = "") -> str:
        if not self.source_url:
            return "fmnolink:"
        if not path:
            return self.source_url
        return "%s/%s/%s" % (self.source_url, kind, "/".join(
            urllib.parse.quote(part, safe="") for part in path.split("/")))

    def raw_url(self, path: str) -> str:
        return "fmrepo:" + urllib.parse.quote(path, safe="/")


class CheckoutSource:
    def __init__(self, root: Path) -> None:
        self.root = Path(root)
        self.commit = _git_text(["git", "-C", str(self.root), "rev-parse", "HEAD"])
        self.tracked: set[str] | None = None

    def list(self, prefix: str) -> list[str]:
        base = self.root / prefix
        if not base.is_dir():
            return []
        found = []
        for directory, _dirs, names in os.walk(base):
            for name in names:
                found.append(Path(directory, name).relative_to(self.root).as_posix())
        return sorted(found)

    def exists(self, rel: str) -> bool:
        return (self.root / rel).is_file()

    def published(self, rel: str) -> bool:
        if self.tracked is None:
            listing = subprocess.run(["git", "-C", str(self.root), "ls-files", "-s", "-z"],
                                     capture_output=True)
            self.tracked = set()
            for entry in listing.stdout.split(b"\0") if listing.returncode == 0 else ():
                meta, _, path = entry.partition(b"\t")
                if meta.split()[:1] in ([b"100644"], [b"100755"]):
                    self.tracked.add(path.decode("utf-8", "surrogateescape"))
        target = self.root / rel
        if rel not in self.tracked or target.is_symlink() or not target.is_file():
            return False
        try:
            target.resolve().relative_to(self.root.resolve())
        except ValueError:
            return False
        return True

    def read(self, rel: str) -> bytes:
        return (self.root / rel).read_bytes()

    def size(self, rel: str) -> int:
        return (self.root / rel).stat().st_size

    def close(self) -> None:
        pass


class GitSource:
    def __init__(self, git_dir: Path, ref: str) -> None:
        self.git = ["git", "--git-dir", str(git_dir)]
        commit = _git_text(self.git + ["rev-parse", "--verify", "--quiet", ref + "^{commit}"])
        if not commit:
            raise ExportError("%s has no commit %s" % (git_dir, ref))
        self.commit = commit
        listing = subprocess.run(self.git + ["ls-tree", "-r", "-z", "--long", "--full-tree", commit],
                                 capture_output=True, check=True).stdout
        self.blobs: dict[str, tuple[str, int]] = {}
        self.links: set[str] = set()
        for entry in listing.split(b"\0"):
            if not entry:
                continue
            meta, _, path = entry.partition(b"\t")
            fields = meta.split()
            if len(fields) == 4 and fields[1] == b"blob":
                name = path.decode("utf-8", "surrogateescape")
                self.blobs[name] = (fields[2].decode(), int(fields[3]))
                if fields[0] == b"120000":
                    self.links.add(name)
        self.proc: subprocess.Popen | None = None

    def list(self, prefix: str) -> list[str]:
        start = prefix.rstrip("/") + "/"
        return sorted(path for path in self.blobs if path.startswith(start))

    def exists(self, rel: str) -> bool:
        return rel in self.blobs

    def published(self, rel: str) -> bool:
        return rel in self.blobs and rel not in self.links

    def size(self, rel: str) -> int:
        return self.blobs[rel][1]

    def read(self, rel: str) -> bytes:
        if rel not in self.blobs:
            raise FileNotFoundError(rel)
        if self.proc is None:
            self.proc = subprocess.Popen(self.git + ["cat-file", "--batch"], stdin=subprocess.PIPE,
                                         stdout=subprocess.PIPE)
        assert self.proc.stdin and self.proc.stdout
        self.proc.stdin.write(self.blobs[rel][0].encode() + b"\n")
        self.proc.stdin.flush()
        header = self.proc.stdout.readline().split()
        if len(header) != 3:
            raise ExportError("git cat-file could not read %s" % rel)
        data = self.proc.stdout.read(int(header[2]))
        self.proc.stdout.read(1)
        return data

    def close(self) -> None:
        if self.proc is not None:
            assert self.proc.stdin
            self.proc.stdin.close()
            self.proc.wait()
            self.proc = None


def _git_text(argv: list[str]) -> str:
    try:
        result = subprocess.run(argv, capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return result.stdout.strip() if result.returncode == 0 else ""


def open_source(path: Path, ref: str = "") -> CheckoutSource | GitSource:
    path = Path(path)
    if ref:
        git_dir = path / ".git" if (path / ".git").exists() else path
        return GitSource(git_dir, ref)
    if (path / ISSUES_REL).is_dir() or (path / ".git").exists() or not (path / "HEAD").is_file():
        return CheckoutSource(path)
    return GitSource(path, "HEAD")


def _ts(value: Any) -> int:
    try:
        number = int(float(value or 0))
    except (TypeError, ValueError):
        return 0
    return number * 1000 if 0 < number < 100_000_000_000 else number


def fold(record: dict[str, Any], side: str) -> dict[str, Any] | None:
    events = sorted((ev for ev in record.get("events") or [] if isinstance(ev, dict)),
                    key=lambda ev: (_ts(ev.get("ts")), str(ev.get("id", ""))))
    opener = next((ev for ev in events if ev.get("type") == "open"), {})
    creator = opener.get("author") or record.get("author") or ""
    if any(ev.get("type") == "delete" and ev.get("target") == "self" and ev.get("author")
           and ev.get("author") == creator for ev in events):
        return None
    title = record.get("title") or opener.get("title") or ""
    status = record.get("status") or record.get("state") or ("closed" if side == "closed" else "open")
    labels = record.get("labels") if isinstance(record.get("labels"), list) else []
    edits: dict[str, str] = {}
    deleted: set[str] = set()
    for ev in events:
        kind = ev.get("type")
        if kind == "title" and ev.get("title"):
            title = ev["title"]
        elif kind == "status" and ev.get("status"):
            status = ev["status"]
        elif kind == "labels" and isinstance(ev.get("labels"), list):
            labels = ev["labels"]
        elif kind == "edit" and ev.get("target"):
            edits[str(ev["target"])] = str(ev.get("body") or "")
        elif kind == "delete" and ev.get("target") and ev.get("target") != "self":
            deleted.add(str(ev["target"]))
    opener_id = str(opener.get("id") or "")
    number = int(record.get("number") or 0)
    comments = []
    for ev in events:
        if ev.get("type") != "comment" or not ev.get("id") or str(ev["id"]) in deleted:
            continue
        comments.append({"id": str(ev["id"]), "author": str(ev.get("authorName") or ev.get("author") or ""),
                         "ts": _ts(ev.get("ts")), "body": edits.get(str(ev["id"]), str(ev.get("body") or ""))})
    created = _ts(record.get("createdAt") or opener.get("ts"))
    return {
        "number": number,
        "title": str(title or "Thread #%d" % number),
        "status": str(status),
        "labels": sorted({str(label) for label in labels if str(label).strip()}, key=str.casefold),
        "author": str(record.get("authorName") or opener.get("authorName") or creator or "unknown"),
        "createdAt": created,
        "updatedAt": max([created] + [_ts(ev.get("ts")) for ev in events]),
        "body": edits.get(opener_id, str(opener.get("body") or record.get("body") or "")),
        "comments": comments,
    }


def load_threads(source: CheckoutSource | GitSource) -> tuple[list[dict[str, Any]], dict[int, bytes]]:
    found: dict[int, tuple[str, str]] = {}
    pattern = re.compile(r"^\.forkmesh/issues/(?:(open|closed)/)?([1-9][0-9]{0,8})/issue-\2\.json$")
    for rel in source.list(ISSUES_REL):
        match = pattern.match(rel)
        if match:
            number = int(match.group(2))
            if number not in found or match.group(1) == "closed":
                found[number] = (rel, match.group(1) or "")
    threads, raw = [], {}
    for number in sorted(found):
        rel, side = found[number]
        data = source.read(rel)
        try:
            record = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            continue
        if not isinstance(record, dict):
            continue
        record.setdefault("number", number)
        thread = fold(record, side)
        if thread is None:
            continue
        thread["number"] = number
        threads.append(thread)
        raw[number] = data
    return threads, raw


def label_slugs(labels: Iterable[str]) -> dict[str, str]:
    slugs: dict[str, str] = {}
    used: set[str] = set()
    for label in sorted(set(labels), key=lambda value: (value.casefold(), value)):
        base = re.sub(r"[^a-z0-9]+", "-", label.casefold()).strip("-")[:60]
        if not base or base != re.sub(r"[^a-z0-9]+", "-", label.casefold()).strip("-"):
            base = (base + "-" if base else "") + hashlib.sha256(label.encode()).hexdigest()[:8]
        slug, n = base, 2
        while slug in used:
            slug, n = "%s-%d" % (base, n), n + 1
        used.add(slug)
        slugs[label] = slug
    return slugs


def iso(ms: int) -> str:
    return datetime.datetime.fromtimestamp(max(ms, 0) / 1000, datetime.timezone.utc).strftime(
        "%Y-%m-%dT%H:%M:%SZ")


def day(ms: int) -> str:
    return iso(ms)[:10] if ms else ""


def person(name: str) -> tuple[str, str]:
    match = FORGE_NAME_RE.match(name.strip())
    if match:
        return match.group(2), "%s-%s.png" % (match.group(1), match.group(2).lower())
    text = name.strip() or "unknown"
    if re.fullmatch(r"[0-9a-fA-F]{40,}|[A-Za-z0-9_-]{43,}", text):
        text = text[:8] + "\u2026"
    return text, ""


class Exporter:
    def __init__(self, source: CheckoutSource | GitSource, out: Path, *, title: str = "",
                 source_url: str = "", base_url: str = "", site_id: str = "",
                 latest: int = DEFAULT_LATEST, feed_size: int = DEFAULT_FEED_SIZE,
                 avatars: bool = True, max_attachment: int = DEFAULT_MAX_ATTACHMENT,
                 renderer: Any = "auto") -> None:
        self.source = source
        self.out = Path(out)
        self.source_url = source_url.rstrip("/")
        self.base_url = base_url.rstrip("/") + "/" if base_url else ""
        self.site_id = site_id or "forum"
        self.latest = latest
        self.feed_size = feed_size
        self.avatars = avatars
        self.max_attachment = max_attachment
        self.renderer = _host_renderer() if renderer == "auto" else renderer
        self.title = title
        self.files: dict[str, bytes | None] = {}
        self.copies: dict[str, str] = {}
        self.inputs: dict[str, str] = {}
        self.stats = {"rendered": 0, "reused": 0}
        self.attachments = self._attachments()
        self.avatar_files = {rel.rsplit("/", 1)[1] for rel in source.list(CONTRIBUTORS_REL)
                             if AVATAR_RE.match(rel.rsplit("/", 1)[1])} if avatars else set()
        origin = urllib.parse.urlsplit(self.source_url)
        self.source_origin = "%s://%s" % (origin.scheme, origin.netloc) if origin.netloc else ""

    def _attachments(self) -> dict[str, dict[str, Any]]:
        if not self.source.exists(ATTACHMENT_INDEX_REL):
            return {}
        try:
            doc = json.loads(self.source.read(ATTACHMENT_INDEX_REL).decode("utf-8"))
        except (UnicodeDecodeError, ValueError):
            return {}
        files = doc.get("files") if isinstance(doc, dict) else None
        found = {}
        for url, entry in (files or {}).items():
            if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
                continue
            match = STORED_RE.match(entry["path"])
            if match and self.source.exists(entry["path"]) and \
                    self.source.size(entry["path"]) <= self.max_attachment:
                found[str(url)] = {"path": entry["path"], "name": match.group(1),
                                   "type": str(entry.get("type") or "")}
        return found

    def markdown(self, text: str, ctx: StaticContext, headings: bool = False) -> str:
        if self.renderer is not None:
            markup = self.renderer.render_markdown(text, ctx)
        else:
            markup = FallbackMarkdown(ctx).render(text)
        return self.slim(markup, ctx.prefix, headings)

    def slim(self, markup: str, prefix: str, headings: bool) -> str:
        markup = SVG_RE.sub("", markup)

        def tidy(match: re.Match) -> str:
            tag = match.group(0)
            if headings:
                tag = HEADING_ID_RE.sub(r' id="\1"', tag)
            return DROP_ATTR_RE.sub("", tag)
        markup = OPEN_TAG_RE.sub(tidy, markup)
        out, pos, depth, dropped = [], 0, 0, []
        for match in TAG_RE.finditer(markup):
            out.append(markup[pos:match.start()])
            pos = match.end()
            closing, tag, attrs = match.group(1), match.group(2).lower(), match.group(3)
            values = {k: html.unescape(v) for k, v in ATTR_RE.findall(attrs)}
            if tag == "a" and closing:
                if dropped and dropped[-1]:
                    dropped.pop()
                    continue
                if dropped:
                    dropped.pop()
                depth = max(depth - 1, 0)
                out.append("</a>")
                continue
            if tag == "a":
                href = self.local_href(values.get("href", ""), prefix)
                if not href:
                    dropped.append(True)
                    continue
                dropped.append(False)
                depth += 1
                rel = ' rel="nofollow ugc"' if re.match(r"^https?:", href, re.I) else ""
                out.append('<a href="%s"%s>' % (esc(href), rel))
                continue
            src = values.get("src", "")
            alt = values.get("alt", "")
            local = self.local_image(src, prefix)
            if local:
                out.append('<img src="%s" alt="%s" loading="lazy">' % (esc(local), esc(alt)))
                continue
            label = "[image%s]" % (": " + alt if alt else "")
            if depth or not re.match(r"^https?:", src, re.I):
                out.append(esc(label))
            else:
                out.append('<a href="%s" rel="nofollow noreferrer">%s</a>' % (esc(src), esc(label)))
        out.append(markup[pos:])
        return "".join(out)

    def attachment(self, url: str) -> dict[str, Any] | None:
        text = url.strip().replace("&amp;", "&")
        while text and text[-1] in ".,:;!?":
            text = text[:-1]
        return self.attachments.get(text)

    def local_href(self, href: str, prefix: str) -> str:
        if not href or href.startswith("fmnolink:"):
            return ""
        if href.startswith("fmrepo:"):
            return self.repo_file(urllib.parse.unquote(href[7:]), prefix) or ""
        entry = self.attachment(href)
        if entry:
            self.copies["files/" + entry["name"]] = entry["path"]
            return prefix + "files/" + entry["name"]
        if href.startswith("//"):
            return "https:" + href
        if href.startswith("/"):
            return self.source_origin + href if self.source_origin else ""
        return href

    def local_image(self, src: str, prefix: str) -> str:
        if not src:
            return ""
        if src.startswith("data:image/"):
            return src
        if src.startswith("fmrepo:"):
            return self.repo_file(urllib.parse.unquote(src[7:]), prefix)
        entry = self.attachment(src)
        if entry and entry["name"].rsplit(".", 1)[1] in IMAGE_TYPES:
            self.copies["files/" + entry["name"]] = entry["path"]
            return prefix + "files/" + entry["name"]
        return ""

    def repo_file(self, path: str, prefix: str) -> str:
        parts = [part for part in path.split("/") if part not in ("", ".")]
        if not parts or ".." in parts or parts[0] == ".git":
            return ""
        rel = "/".join(parts)
        if not self.source.published(rel) or self.source.size(rel) > self.max_attachment:
            return ""
        self.copies["repo/" + rel] = rel
        return prefix + "repo/" + "/".join(urllib.parse.quote(part) for part in parts)

    def avatar(self, file: str, prefix: str) -> str:
        if not file or file not in self.avatar_files:
            return ""
        self.copies["avatars/" + file] = CONTRIBUTORS_REL + "/" + file
        return '<img src="%savatars/%s" width="20" height="20" alt="" loading="lazy">' % (prefix, file)

    def page(self, title: str, prefix: str, body: str) -> bytes:
        site = esc(self.title)
        head = esc(title) + (" \u00b7 " + site if title != self.title else "")
        return ("<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                "<title>%s</title><link rel=\"stylesheet\" href=\"%sstyle.css\">"
                "<link rel=\"alternate\" type=\"application/atom+xml\" title=\"%s\" href=\"%sfeed.atom\">"
                "</head><body><header><b><a href=\"%sindex.html\">%s</a></b> \u00b7 "
                "<a href=\"%sall.html\">All threads</a> \u00b7 <a href=\"%sfeed.atom\">Atom feed</a>"
                "%s</header>\n%s\n<footer>A static, read-only copy%s. "
                "<a href=\"%smanifest.json\">manifest.json</a> lists every file and its SHA-256."
                "</footer></body></html>\n" % (
                    head, prefix, site, prefix, prefix, site, prefix, prefix,
                    (' \u00b7 <a href="%s">Live forum</a>' % esc(self.source_url)) if self.source_url else "",
                    body, (" of " + esc(self.source_url)) if self.source_url else "",
                    prefix)).encode("utf-8")

    def thread_row(self, thread: dict[str, Any], prefix: str, slugs: dict[str, str]) -> str:
        count = len(thread["comments"])
        labels = " \u00b7 ".join('<a href="%slabels/%s.html">%s</a>' % (prefix, slugs[label], esc(label))
                                 for label in thread["labels"])
        return ('<li><a href="%sissues/%d.html" dir="auto">%s</a><span class="m">#%d%s \u00b7 %d '
                'comment%s \u00b7 %s%s</span></li>' % (
                    prefix, thread["number"], esc(thread["title"]), thread["number"],
                    ' \u00b7 <span class="x">closed</span>' if thread["status"] == "closed" else "",
                    count, "" if count == 1 else "s", day(thread["updatedAt"]),
                    (" \u00b7 " + labels) if labels else ""))

    def thread_list(self, threads: list[dict[str, Any]], prefix: str, slugs: dict[str, str]) -> str:
        return '<ol class="t">%s</ol>' % "".join(self.thread_row(t, prefix, slugs) for t in threads)

    def post(self, anchor: str, author: str, ts: int, body: str, ctx: StaticContext) -> str:
        name, avatar = person(author)
        return ('<article class="post" id="%s"><div class="by">%s<b>%s</b> \u00b7 '
                '<a href="#%s"><time datetime="%s">%s</time></a></div><div class="md" dir="auto">%s</div>'
                '</article>' % (anchor, self.avatar(avatar, ctx.prefix), esc(name), anchor, iso(ts),
                                iso(ts)[:16].replace("T", " "), self.markdown(body, ctx)))

    @staticmethod
    def anchor(comment_id: str) -> str:
        return "c-" + (re.sub(r"[^A-Za-z0-9_-]", "", comment_id)[:24] or "x")

    def thread_page(self, thread: dict[str, Any], numbers: frozenset[int], slugs: dict[str, str]) -> bytes:
        ctx = StaticContext("../", numbers, self.source_url)
        labels = "".join('<a class="lb" href="../labels/%s.html">%s</a>' % (slugs[label], esc(label))
                         for label in thread["labels"])
        posts = [self.post("c0", thread["author"], thread["createdAt"], thread["body"], ctx)]
        posts += [self.post(self.anchor(c["id"]), c["author"], c["ts"], c["body"], ctx)
                  for c in thread["comments"]]
        original = (' \u00b7 <a href="%s/issues/%d">live thread</a>' % (esc(self.source_url), thread["number"])
                    if self.source_url else "")
        body = ('<main><h1 dir="auto">%s</h1><p class="m">#%d \u00b7 %s \u00b7 %d comment%s%s</p>%s%s</main>' % (
            esc(thread["title"]), thread["number"],
            '<span class="x">closed</span>' if thread["status"] == "closed" else "open",
            len(thread["comments"]), "" if len(thread["comments"]) == 1 else "s", original,
            "<p>%s</p>" % labels if labels else "", "".join(posts)))
        return self.page(thread["title"], "../", body)

    def thread_input(self, thread: dict[str, Any], raw: bytes, numbers: frozenset[int]) -> str:
        digest = hashlib.sha256()
        digest.update(json.dumps([EXPORT_VERSION, self.title, self.source_url, self.avatars,
                                  self.renderer is not None], sort_keys=True).encode())
        digest.update(hashlib.sha256(raw).digest())
        text = raw.decode("utf-8", "replace")
        refs = sorted({int(n) for n in REF_RE.findall(text) if int(n) in numbers})
        urls = sorted({u for u in URL_RE.findall(text.replace("\\/", "/")) if self.attachment(u)})
        people = sorted({person(name)[1] for name in [thread["author"]] + [c["author"] for c in thread["comments"]]}
                        & self.avatar_files)
        digest.update(json.dumps([refs, [self.attachment(u)["name"] for u in urls], people]).encode())
        return digest.hexdigest()

    def thread_copies(self, page: bytes) -> list[str]:
        found = re.findall(rb'(?:src|href)="\.\./((?:files|avatars|repo)/[^"#?]+)"', page)
        return sorted({urllib.parse.unquote(item.decode()) for item in found})

    def readme(self) -> tuple[str, str]:
        for name in README_NAMES:
            if self.source.exists(name):
                text = self.source.read(name).decode("utf-8", "replace")
                heading = re.search(r"^#\s+(.+?)\s*#*\s*$", text, re.M)
                return text, heading.group(1).strip() if heading else ""
        return "", ""

    def feed(self, threads: list[dict[str, Any]], numbers: frozenset[int]) -> bytes:
        ctx = StaticContext("", numbers, self.source_url)
        tag = "tag:forkmesh.com,2026:static/%s" % urllib.parse.quote(self.site_id, safe="/")
        entries = []
        for thread in threads:
            entries.append((thread["createdAt"], 0, thread["number"], "", thread))
            for comment in thread["comments"]:
                entries.append((comment["ts"], 1, thread["number"], comment["id"], comment))
        entries.sort(key=lambda e: (e[0], e[1], e[2], e[3]), reverse=True)
        entries = entries[:self.feed_size]
        by_number = {t["number"]: t for t in threads}
        updated = max([e[0] for e in entries], default=0)
        out = ['<?xml version="1.0" encoding="utf-8"?>\n<feed xmlns="http://www.w3.org/2005/Atom" '
               'xmlns:thr="http://purl.org/syndication/thread/1.0"%s>' % (
                   ' xml:base="%s"' % esc(self.base_url) if self.base_url else ""),
               "<id>%s</id><title>%s</title><updated>%s</updated>" % (esc(tag), esc(self.title), iso(updated)),
               '<link rel="self" href="feed.atom"/><link rel="alternate" type="text/html" href="index.html"/>',
               "<generator>%s</generator>" % GENERATOR]
        for ts, kind, number, cid, item in entries:
            thread = by_number[number]
            thread_id = "%s/issue/%d" % (tag, number)
            page = "issues/%d.html" % number
            name, _ = person(item["author"])
            content = self.markdown(item["body"], ctx)
            if kind == 0:
                out.append('<entry><id>%s</id><title>%s</title><updated>%s</updated><published>%s</published>'
                           '<author><name>%s</name></author><link rel="alternate" type="text/html" href="%s"/>'
                           '%s<content type="html">%s</content></entry>' % (
                               esc(thread_id), esc(thread["title"]), iso(ts), iso(ts), esc(name), page,
                               "".join('<category term="%s"/>' % esc(label) for label in thread["labels"]),
                               esc(content)))
            else:
                anchor = self.anchor(cid)
                out.append('<entry><id>%s/comment/%s</id><title>Re: %s</title><updated>%s</updated>'
                           '<published>%s</published><author><name>%s</name></author>'
                           '<link rel="alternate" type="text/html" href="%s#%s"/>'
                           '<thr:in-reply-to ref="%s" type="text/html" href="%s"/>'
                           '<content type="html">%s</content></entry>' % (
                               esc(thread_id), esc(urllib.parse.quote(cid, safe="")), esc(thread["title"]),
                               iso(ts), iso(ts), esc(name), page, anchor, esc(thread_id), page, esc(content)))
        out.append("</feed>\n")
        return "\n".join(out).encode("utf-8")

    def previous(self) -> dict[str, Any]:
        try:
            doc = json.loads((self.out / STATE_FILE).read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        return doc if isinstance(doc, dict) and doc.get("schema") == MANIFEST_SCHEMA else {}

    def run(self) -> dict[str, Any]:
        threads, raw = load_threads(self.source)
        readme, readme_title = self.readme()
        self.title = self.title or readme_title or self.site_id
        numbers = frozenset(t["number"] for t in threads)
        slugs = label_slugs(label for t in threads for label in t["labels"])
        recent = sorted(threads, key=lambda t: (t["updatedAt"], t["number"]), reverse=True)
        previous = self.previous()
        old_files = previous.get("files") if isinstance(previous.get("files"), dict) else {}
        old_inputs = previous.get("inputs") if isinstance(previous.get("inputs"), dict) else {}
        old_copies = previous.get("threadFiles") if isinstance(previous.get("threadFiles"), dict) else {}
        thread_files: dict[str, list[str]] = {}
        for thread in threads:
            rel = "issues/%d.html" % thread["number"]
            key = self.thread_input(thread, raw[thread["number"]], numbers)
            self.inputs[rel] = key
            old = old_files.get(rel)
            if (old_inputs.get(rel) == key and isinstance(old, dict) and (self.out / rel).is_file()
                    and _sha256_file(self.out / rel) == old.get("sha256")):
                self.files[rel] = None
                copies = [c for c in old_copies.get(rel, []) if isinstance(c, str)]
                for item in copies:
                    self._copy_source(item)
                thread_files[rel] = copies
                self.stats["reused"] += 1
                continue
            page = self.thread_page(thread, numbers, slugs)
            self.files[rel] = page
            thread_files[rel] = self.thread_copies(page)
            self.stats["rendered"] += 1
        ctx = StaticContext("", numbers, self.source_url)
        label_counts = {label: sum(1 for t in threads if label in t["labels"]) for label in slugs}
        label_index = '<p>%s</p>' % " ".join(
            '<a class="lb" href="labels/%s.html">%s (%d)</a>' % (slugs[label], esc(label), label_counts[label])
            for label in sorted(slugs, key=lambda value: (-label_counts[value], value.casefold())))
        intro = '<div class="md" dir="auto">%s</div>' % self.markdown(readme, ctx, True) if readme.strip() else ""
        self.files["index.html"] = self.page(self.title, "", (
            '<main>%s<h2>Latest threads</h2>%s<p><a href="all.html">All %d threads</a></p>%s</main>' % (
                intro, self.thread_list(recent[:self.latest], "", slugs), len(threads),
                ("<h2>Labels</h2>" + label_index) if slugs else "")))
        self.files["all.html"] = self.page("All threads", "", (
            '<main><h1>All threads</h1><p class="m">%d threads, newest activity first. '
            'Use your browser\'s find (Ctrl+F) to search titles.</p>%s</main>' % (
                len(threads), self.thread_list(recent, "", slugs))))
        for label, slug in slugs.items():
            tagged = [t for t in recent if label in t["labels"]]
            self.files["labels/%s.html" % slug] = self.page(label, "../", (
                '<main><h1 dir="auto">%s</h1><p class="m">%d threads</p>%s</main>' % (
                    esc(label), len(tagged), self.thread_list(tagged, "../", slugs))))
        self.files["feed.atom"] = self.feed(threads, numbers)
        self.files["style.css"] = CSS.encode("utf-8")
        self.files[".nojekyll"] = b""
        return self.write(previous, old_files, thread_files, threads)

    def _copy_source(self, rel: str) -> None:
        if rel.startswith("files/"):
            name = rel[6:]
            if STORED_RE.match(ATTACHMENTS_REL + "/" + name) and self.source.exists(ATTACHMENTS_REL + "/" + name):
                self.copies[rel] = ATTACHMENTS_REL + "/" + name
        elif rel.startswith("avatars/"):
            if rel[8:] in self.avatar_files:
                self.copies[rel] = CONTRIBUTORS_REL + "/" + rel[8:]
        elif rel.startswith("repo/"):
            if self.source.exists(rel[5:]):
                self.copies[rel] = rel[5:]

    def write(self, previous: dict[str, Any], old_files: dict[str, Any], thread_files: dict[str, list[str]],
              threads: list[dict[str, Any]]) -> dict[str, Any]:
        self.out.mkdir(parents=True, exist_ok=True)
        manifest_files: dict[str, dict[str, Any]] = {}
        written = 0
        for rel in sorted(self.files):
            data = self.files[rel]
            target = _inside(self.out, rel)
            if data is None:
                manifest_files[rel] = old_files[rel]
                continue
            if _write_if_changed(target, data):
                written += 1
            manifest_files[rel] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
        for rel in sorted(self.copies):
            if rel in manifest_files:
                continue
            target = _inside(self.out, rel)
            old = old_files.get(rel)
            if isinstance(old, dict) and target.is_file() and target.stat().st_size == old.get("bytes") \
                    and rel.startswith("files/"):
                manifest_files[rel] = old
                continue
            data = self.source.read(self.copies[rel])
            if _write_if_changed(target, data):
                written += 1
            manifest_files[rel] = {"sha256": hashlib.sha256(data).hexdigest(), "bytes": len(data)}
        removed = 0
        for rel in sorted(set(old_files) - set(manifest_files)):
            target = _inside(self.out, rel)
            if target.is_file():
                target.unlink()
                removed += 1
                _prune_empty(target.parent, self.out)
        generated = max([t["updatedAt"] for t in threads], default=0)
        manifest = {
            "schema": MANIFEST_SCHEMA,
            "generator": GENERATOR,
            "title": self.title,
            "site": self.site_id,
            "source": {"commit": self.source.commit, "url": self.source_url},
            "updated": iso(generated) if generated else "",
            "threads": len(threads),
            "comments": sum(len(t["comments"]) for t in threads),
            "files": manifest_files,
            "inputs": dict(sorted(self.inputs.items())),
            "threadFiles": {rel: files for rel, files in sorted(thread_files.items()) if files},
        }
        data = (json.dumps(manifest, indent=1, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")
        _write_if_changed(self.out / STATE_FILE, data)
        total = sum(entry["bytes"] for entry in manifest_files.values()) + len(data)
        return {"ok": True, "out": str(self.out), "commit": self.source.commit, "threads": len(threads),
                "comments": manifest["comments"], "files": len(manifest_files) + 1, "bytes": total,
                "written": written, "removed": removed, **self.stats}


def _inside(root: Path, rel: str) -> Path:
    parts = rel.split("/")
    if not rel or rel.startswith("/") or any(part in ("", ".", "..") for part in parts):
        raise ExportError("refusing output path %r" % rel)
    return root.joinpath(*parts)


def _prune_empty(directory: Path, root: Path) -> None:
    while directory != root and root in directory.parents:
        try:
            directory.rmdir()
        except OSError:
            return
        directory = directory.parent


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def _write_if_changed(path: Path, data: bytes) -> bool:
    if path.is_file() and path.stat().st_size == len(data) and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(path.name + ".tmp")
    temp.write_bytes(data)
    temp.replace(path)
    return True


def write_tarball(site: Path, target: Path, prefix: str = "site") -> int:
    manifest = json.loads((site / STATE_FILE).read_text(encoding="utf-8"))
    names = sorted(set(manifest.get("files") or {}) | {STATE_FILE})
    buffer = io.BytesIO()
    with gzip.GzipFile(fileobj=buffer, mode="wb", mtime=0, filename="") as zipped:
        with tarfile.open(fileobj=zipped, mode="w", format=tarfile.PAX_FORMAT) as tar:
            for rel in names:
                data = _inside(site, rel).read_bytes()
                info = tarfile.TarInfo("%s/%s" % (prefix, rel))
                info.size, info.mode, info.mtime = len(data), 0o644, 0
                info.uname = info.gname = ""
                tar.addfile(info, io.BytesIO(data))
    _write_if_changed(Path(target), buffer.getvalue())
    return len(buffer.getvalue())


def verify(site: Path) -> list[str]:
    manifest = json.loads((site / STATE_FILE).read_text(encoding="utf-8"))
    problems = []
    for rel, entry in sorted((manifest.get("files") or {}).items()):
        path = _inside(site, rel)
        if not path.is_file():
            problems.append("missing " + rel)
        elif _sha256_file(path) != entry.get("sha256"):
            problems.append("changed " + rel)
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Write a static, read-only copy of a ForkMesh forum "
                                                 "(the issues in .forkmesh/issues) that any static host can serve.")
    parser.add_argument("--source", type=Path, help="checkout, or bare repository with --ref")
    parser.add_argument("--ref", default="", help="read this commit from the repository instead of the files")
    parser.add_argument("--out", type=Path, required=True, help="site folder to write (updated in place)")
    parser.add_argument("--title", default="", help="site title (default: the README's first heading)")
    parser.add_argument("--source-url", default="", help="live forum repository URL, linked from every page")
    parser.add_argument("--base-url", default="", help="public URL of the static copy (xml:base of the feed)")
    parser.add_argument("--site-id", default="", help="stable feed id, for example example/forum")
    parser.add_argument("--latest", type=int, default=DEFAULT_LATEST)
    parser.add_argument("--feed-size", type=int, default=DEFAULT_FEED_SIZE)
    parser.add_argument("--no-avatars", action="store_true")
    parser.add_argument("--max-attachment-bytes", type=int, default=DEFAULT_MAX_ATTACHMENT)
    parser.add_argument("--fallback-renderer", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--tarball", type=Path, help="also write a reproducible .tar.gz of the site here")
    parser.add_argument("--verify", action="store_true", help="only check --out against its manifest.json")
    args = parser.parse_args(argv)
    if args.verify:
        problems = verify(args.out)
        print(json.dumps({"ok": not problems, "problems": problems}))
        return 0 if not problems else 1
    if args.source is None:
        parser.error("--source is required")
    try:
        source = open_source(args.source, args.ref)
    except (ExportError, subprocess.CalledProcessError) as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 2
    try:
        result = Exporter(source, args.out, title=args.title, source_url=args.source_url,
                          base_url=args.base_url, site_id=args.site_id, latest=args.latest,
                          feed_size=args.feed_size, avatars=not args.no_avatars,
                          max_attachment=args.max_attachment_bytes,
                          renderer=None if args.fallback_renderer else "auto").run()
    except ExportError as error:
        print(json.dumps({"ok": False, "error": str(error)}))
        return 2
    finally:
        source.close()
    if args.tarball:
        result["tarballBytes"] = write_tarball(args.out, args.tarball)
    print(json.dumps(result, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
