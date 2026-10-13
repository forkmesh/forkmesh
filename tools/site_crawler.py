#!/usr/bin/env python3
import argparse
import json
import re
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import urllib.robotparser
import xml.etree.ElementTree as ET
from collections import defaultdict, deque
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field, replace
from datetime import datetime, timezone
from html.parser import HTMLParser
from pathlib import Path

PROJECT = Path(__file__).resolve().parents[1]
DEFAULT_START = "https://forkmesh.com/"
USER_AGENT = "ForkMeshSiteCheck/1.0 (+https://forkmesh.com)"
TIMEOUT = 20.0
MAX_REDIRECTS = 10
MAX_BODY = 3 * 1024 * 1024
DEFAULT_MAX_PAGES = 600
DEFAULT_TIME_BUDGET = 480.0
DEFAULT_WORKERS = 4
DEFAULT_RATE = 6.0
RATE_LIMIT_ATTEMPTS = 3
RETRY_AFTER_DEFAULT = 10.0
RETRY_AFTER_CAP = 30.0
DEFAULT_EXAMPLES = 8
SLOW_SECONDS = 1.5
VERY_SLOW_SECONDS = 3.0
HEAVY_HTML_BYTES = 250 * 1024
TITLE_RANGE = (15, 70)
DESCRIPTION_RANGE = (50, 170)
SEVERITIES = ("error", "warning", "notice")
REDIRECT_CODES = {301, 302, 303, 307, 308}
UNVERIFIABLE_CODES = {401, 403, 405, 429, 999}
PAGE_SUFFIXES = {"", ".html", ".htm", ".php", ".md"}
SKIP_SCHEMES = ("mailto:", "tel:", "javascript:", "data:", "sms:", "blob:", "about:")
GENERIC_LINK_TEXT = {"click here", "here", "read more", "more", "link", "this", "learn more", "this page"}
SECURITY_HEADERS = (
    "strict-transport-security",
    "content-security-policy",
    "x-content-type-options",
    "referrer-policy",
)
OG_PROPERTIES = ("og:title", "og:description", "og:image")
SITEMAP_NS = "{http://www.sitemaps.org/schemas/sitemap/0.9}"


@dataclass
class Response:
    url: str
    final_url: str
    status: int | None
    headers: dict
    body: bytes
    elapsed: float
    hops: list = field(default_factory=list)
    error: str = ""

    @property
    def content_type(self):
        return self.headers.get("content-type", "").split(";")[0].strip().lower()

    @property
    def is_html(self):
        return "html" in self.content_type

    def text(self):
        match = re.search(r"charset=([\w-]+)", self.headers.get("content-type", ""), re.IGNORECASE)
        encoding = match.group(1) if match else "utf-8"
        try:
            return self.body.decode(encoding, errors="replace")
        except LookupError:
            return self.body.decode("utf-8", errors="replace")


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


_OPENER = urllib.request.build_opener(_NoRedirect)


def fetch(url, method="GET", limit=MAX_BODY, timeout=TIMEOUT):
    started = time.monotonic()
    hops = []
    current = url
    for _ in range(MAX_REDIRECTS + 1):
        request = urllib.request.Request(
            current,
            method=method,
            headers={"User-Agent": USER_AGENT, "Accept": "text/html,application/xhtml+xml,*/*;q=0.8"},
        )
        try:
            response = _OPENER.open(request, timeout=timeout)
        except urllib.error.HTTPError as error:
            response = error
        except (urllib.error.URLError, OSError, ValueError) as error:
            reason = getattr(error, "reason", error)
            return Response(url, current, None, {}, b"", time.monotonic() - started, hops, str(reason))
        status = getattr(response, "status", None) or getattr(response, "code", None)
        headers = {key.lower(): value for key, value in (response.headers or {}).items()}
        if status in REDIRECT_CODES and headers.get("location"):
            response.close()
            hops.append({"status": status, "url": current})
            current = urllib.parse.urljoin(current, headers["location"])
            continue
        body = b""
        if method == "GET":
            try:
                body = response.read(limit)
            except (OSError, ValueError):
                body = b""
        response.close()
        return Response(url, current, status, headers, body, time.monotonic() - started, hops)
    return Response(url, current, None, {}, b"", time.monotonic() - started, hops, "too many redirects")


def normalize(url, base=None):
    if base is not None:
        url = urllib.parse.urljoin(base, url)
    parts = urllib.parse.urlsplit(url)
    if parts.scheme not in ("http", "https") or not parts.hostname:
        return None
    host = parts.hostname.lower()
    port = parts.port
    if port and not ((parts.scheme == "http" and port == 80) or (parts.scheme == "https" and port == 443)):
        host = f"{host}:{port}"
    path = parts.path or "/"
    return urllib.parse.urlunsplit((parts.scheme, host, path, parts.query, ""))


def fragment_of(url):
    return urllib.parse.urlsplit(url).fragment


def host_of(url):
    return urllib.parse.urlsplit(url).netloc.lower()


def registrable(host):
    host = host.split(":")[0]
    labels = host.split(".")
    return ".".join(labels[-2:]) if len(labels) >= 2 else host


def looks_like_page(url):
    suffix = Path(urllib.parse.urlsplit(url).path).suffix.lower()
    return suffix in PAGE_SUFFIXES


def source_for(url, root=PROJECT):
    path = urllib.parse.unquote(urllib.parse.urlsplit(url).path).strip("/")
    if path.endswith(".html"):
        path = path[: -len(".html")]
    candidates = []
    if not path:
        candidates.append("www/templates/pages/index.html")
    else:
        candidates += [f"www/templates/pages/{path}.html", f"www/templates/pages/{path}/index.html"]
        if path.startswith("docs/"):
            rest = path[len("docs/"):]
            candidates += [f"www/docs/{rest}.md", f"www/docs/{rest}/README.md", f"www/docs/{rest}/index.md"]
        candidates += [f"www/public/{path}", f"www/public/{path}.html", f"www/public/{path}/index.html"]
    for candidate in candidates:
        if (root / candidate).is_file():
            return candidate
    return ""


class PageParser(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.lang = None
        self.title = None
        self.meta = {}
        self.canonical = None
        self.ids = []
        self.headings = []
        self.anchors = []
        self.images = []
        self.assets = []
        self._title_parts = None
        self._anchor = None
        self._heading = None

    def handle_starttag(self, tag, attrs):
        attributes = {name.lower(): (value if value is not None else "") for name, value in attrs}
        if attributes.get("id"):
            self.ids.append(attributes["id"])
        if tag == "a" and attributes.get("name"):
            self.ids.append(attributes["name"])
        if tag == "html":
            self.lang = attributes.get("lang", "").strip() or None
        elif tag == "title" and self.title is None:
            self._title_parts = []
        elif tag == "meta":
            key = (attributes.get("name") or attributes.get("property") or "").lower()
            if key:
                self.meta.setdefault(key, attributes.get("content", ""))
                if key in ("og:image", "twitter:image") and attributes.get("content"):
                    self.assets.append(("meta-image", attributes["content"]))
        elif tag == "link":
            rel = set(attributes.get("rel", "").lower().split())
            href = attributes.get("href")
            if "canonical" in rel and href is not None and self.canonical is None:
                self.canonical = href
            elif href and rel & {"stylesheet", "icon", "apple-touch-icon", "manifest", "preload", "modulepreload"}:
                self.assets.append(("link", href))
        elif tag == "script" and attributes.get("src"):
            self.assets.append(("script", attributes["src"]))
        elif tag == "img":
            self.images.append(attributes)
            if attributes.get("src"):
                self.assets.append(("img", attributes["src"]))
            if self._anchor is not None and attributes.get("alt", "").strip():
                self._anchor["image_alt"] = True
        elif tag in ("source", "video", "audio", "iframe") and attributes.get("src"):
            self.assets.append((tag, attributes["src"]))
        elif tag == "a" and "href" in attributes:
            self._anchor = {
                "href": attributes["href"],
                "text": [],
                "label": attributes.get("aria-label", "") or attributes.get("title", ""),
                "image_alt": False,
                "line": self.getpos()[0],
            }
        elif tag == "svg" and self._anchor is not None:
            self._anchor["image_alt"] = self._anchor["image_alt"] or bool(attributes.get("aria-label"))
        if re.fullmatch(r"h[1-6]", tag):
            self._heading = {"level": int(tag[1]), "text": []}

    def handle_endtag(self, tag):
        if tag == "title" and self._title_parts is not None:
            self.title = " ".join("".join(self._title_parts).split())
            self._title_parts = None
        elif tag == "a" and self._anchor is not None:
            self._anchor["text"] = " ".join("".join(self._anchor["text"]).split())
            self.anchors.append(self._anchor)
            self._anchor = None
        elif re.fullmatch(r"h[1-6]", tag) and self._heading is not None:
            self._heading["text"] = " ".join("".join(self._heading["text"]).split())
            self.headings.append(self._heading)
            self._heading = None

    def handle_data(self, data):
        if self._title_parts is not None:
            self._title_parts.append(data)
        if self._anchor is not None:
            self._anchor["text"].append(data)
        if self._heading is not None:
            self._heading["text"].append(data)


def parse_page(html):
    parser = PageParser()
    try:
        parser.feed(html)
        parser.close()
    except (AssertionError, ValueError):
        pass
    if parser._anchor is not None:
        parser._anchor["text"] = " ".join("".join(parser._anchor["text"]).split())
        parser.anchors.append(parser._anchor)
    return parser


def parse_sitemap(xml_text):
    try:
        root = ET.fromstring(xml_text)
    except ET.ParseError:
        return None, []
    kind = root.tag.replace(SITEMAP_NS, "")
    locations = [node.text.strip() for node in root.iter(f"{SITEMAP_NS}loc") if node.text and node.text.strip()]
    if not locations:
        locations = [node.text.strip() for node in root.iter("loc") if node.text and node.text.strip()]
    return kind, locations


class Crawler:
    def __init__(
        self,
        start=DEFAULT_START,
        fetcher=fetch,
        max_pages=DEFAULT_MAX_PAGES,
        time_budget=DEFAULT_TIME_BUDGET,
        workers=DEFAULT_WORKERS,
        external=False,
        clock=time.monotonic,
        sleep=time.sleep,
        rate=DEFAULT_RATE,
        source_root=PROJECT,
        use_sitemap=True,
        include=(),
        follow=True,
    ):
        self.start = normalize(start)
        if self.start is None:
            raise ValueError(f"not an http(s) URL: {start}")
        self.host = host_of(self.start)
        self.site_hosts = {self.host, "www." + self.host if not self.host.startswith("www.") else self.host[4:]}
        self.domain = registrable(self.host)
        self.fetcher = fetcher
        self.max_pages = max_pages
        self.time_budget = time_budget
        self.workers = max(1, workers)
        self.external = external
        self.clock = clock
        self.sleep = sleep
        self.gap = 1.0 / rate if rate > 0 else 0.0
        self._next_slot = 0.0
        self._slot_lock = threading.Lock()
        self.rate_limited = 0
        self.source_root = source_root
        self.use_sitemap = use_sitemap
        self.include = tuple(include)
        self.follow = follow
        self.findings = []
        self.responses = {}
        self.pages = {}
        self.links = defaultdict(set)
        self.fragments = defaultdict(set)
        self.assets = defaultdict(set)
        self.linked_from = defaultdict(set)
        self.sitemap_urls = []
        self.robots = None
        self.robots_sitemaps = []
        self.truncated = False
        self.crawled = 0
        self.unchecked = 0
        self.unverifiable = 0
        self._deadline = None

    def source(self, url):
        return source_for(url, self.source_root) if url and host_of(url) in self.site_hosts else ""

    def add(self, severity, check, url, detail, target="", **extra):
        finding = {"severity": severity, "check": check, "url": url, "detail": detail}
        if target:
            finding["target"] = target
        source = self.source(url)
        if source:
            finding["source"] = source
        finding.update(extra)
        if len(finding.get("pages", [])) > 1:
            finding["sources"] = sorted({self.source(page) for page in finding["pages"]} - {""})
        self.findings.append(finding)

    def add_linked(self, severity, check, target, detail, sources):
        pages = sorted(sources)
        suffix = f"; linked from {len(pages)} pages" if len(pages) > 1 else ""
        self.add(severity, check, pages[0], detail + suffix, target, pages=pages)

    def wait_turn(self):
        if not self.gap:
            return
        with self._slot_lock:
            now = self.clock()
            slot = max(now, self._next_slot)
            self._next_slot = slot + self.gap
        if slot > now:
            self.sleep(slot - now)

    def request(self, url, **kwargs):
        response = None
        for _ in range(RATE_LIMIT_ATTEMPTS):
            self.wait_turn()
            response = self.fetcher(url, **kwargs)
            if response.status != 429:
                return response
            with self._slot_lock:
                self.rate_limited += 1
                self.gap = min(max(self.gap * 2, 0.5), 2.0)
            try:
                delay = float(response.headers.get("retry-after", RETRY_AFTER_DEFAULT))
            except ValueError:
                delay = RETRY_AFTER_DEFAULT
            delay = min(max(delay, 1.0), RETRY_AFTER_CAP)
            if self._deadline is not None and self.clock() + delay >= self._deadline:
                break
            self.sleep(delay)
        return response

    def out_of_time(self):
        return self.clock() >= self._deadline

    def is_site(self, url):
        return host_of(url) in self.site_hosts

    def is_first_party(self, url):
        return registrable(host_of(url)) == self.domain

    def in_scope(self, url):
        if not self.include:
            return True
        return url == self.start or urllib.parse.urlsplit(url).path.startswith(self.include)

    def allowed(self, url):
        if self.robots is None:
            return True
        return self.robots.can_fetch(USER_AGENT, url)

    def run(self):
        started = self.clock()
        self._deadline = started + self.time_budget
        self.load_robots()
        if self.use_sitemap:
            self.load_sitemap()
        self.crawl()
        self.check_targets()
        self.check_soft_404()
        self.check_site_wide()
        return self.report(self.clock() - started)

    def origin(self):
        parts = urllib.parse.urlsplit(self.start)
        return f"{parts.scheme}://{parts.netloc}"

    def load_robots(self):
        response = self.request(self.origin() + "/robots.txt")
        if response.status != 200:
            self.add("warning", "robots-missing", self.origin() + "/robots.txt",
                     f"robots.txt answered {response.status or response.error}")
            return
        parser = urllib.robotparser.RobotFileParser()
        parser.parse(response.text().splitlines())
        self.robots = parser
        self.robots_sitemaps = [line.split(":", 1)[1].strip() for line in response.text().splitlines()
                                if line.lower().startswith("sitemap:")]

    def load_sitemap(self):
        pending = deque(self.robots_sitemaps or [self.origin() + "/sitemap.xml"])
        seen = set()
        while pending and len(seen) < 20:
            location = pending.popleft()
            if location in seen:
                continue
            seen.add(location)
            response = self.request(location)
            if response.status != 200:
                self.add("warning", "sitemap-missing", location, f"sitemap answered {response.status or response.error}")
                continue
            kind, locations = parse_sitemap(response.text())
            if kind is None:
                self.add("warning", "sitemap-invalid", location, "sitemap is not valid XML")
            elif kind == "sitemapindex":
                pending.extend(locations)
            else:
                for loc in locations:
                    url = normalize(loc)
                    if url and url not in self.sitemap_urls:
                        self.sitemap_urls.append(url)

    def crawl(self):
        queue = deque([self.start])
        queued = {self.start}
        for url in self.sitemap_urls:
            if self.is_site(url) and self.in_scope(url) and url not in queued:
                queue.append(url)
                queued.add(url)
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            while queue:
                if self.crawled >= self.max_pages or self.out_of_time():
                    self.truncated = True
                    break
                batch = []
                while queue and len(batch) < self.workers * 2 and self.crawled + len(batch) < self.max_pages:
                    url = queue.popleft()
                    if url not in self.responses and self.allowed(url):
                        batch.append(url)
                self.crawled += len(batch)
                for url, response in zip(batch, pool.map(self.request, batch)):
                    self.responses[url] = response
                    for link in self.process(url, response):
                        if link not in queued:
                            queued.add(link)
                            queue.append(link)

    def process(self, url, response):
        discovered = []
        final = normalize(response.final_url) or url
        if response.hops:
            if not self.is_site(final) or final in self.responses:
                return discovered
            response = replace(response, url=final, hops=[])
            self.responses[final] = response
            url = final
        if response.status != 200 or not response.is_html or not self.is_site(final):
            if challenged(response):
                self.unverifiable += 1
            return discovered
        page = parse_page(response.text())
        self.pages[url] = {"final": final, "parser": page, "response": response}
        self.audit_page(url, response, page)
        for anchor in page.anchors:
            href = anchor["href"].strip()
            if not href or href.lower().startswith(SKIP_SCHEMES):
                continue
            target = normalize(href, final)
            if target is None:
                continue
            fragment = fragment_of(urllib.parse.urljoin(final, href))
            if href.startswith("#") or target == final:
                if fragment:
                    self.fragments[final].add((fragment, url))
                continue
            if href.lower().startswith("http://") and self.is_first_party(target):
                self.add("notice", "insecure-link", url, "first-party link uses http://", href)
            self.links[target].add(url)
            self.linked_from[target].add(url)
            if fragment:
                self.fragments[target].add((fragment, url))
            if self.follow and self.is_site(target) and looks_like_page(target) and self.in_scope(target):
                discovered.append(target)
        for kind, src in page.assets:
            if src.strip().lower().startswith(SKIP_SCHEMES):
                continue
            target = normalize(src.strip(), final)
            if target is None:
                continue
            if src.strip().lower().startswith("http://") and urllib.parse.urlsplit(final).scheme == "https" and kind != "meta-image":
                self.add("warning", "mixed-content", url, f"{kind} loaded over http://", src.strip())
            self.assets[target].add(url)
        if page.canonical is not None:
            canonical = normalize(page.canonical.strip(), final) if page.canonical.strip() else None
            if canonical and canonical != final:
                self.links[canonical].add(url)
        return discovered

    def audit_page(self, url, response, page):
        robots = page.meta.get("robots", "").lower()
        indexable = "noindex" not in robots and "noindex" not in response.headers.get("x-robots-tag", "").lower()
        self.pages[url]["indexable"] = indexable
        if response.elapsed >= VERY_SLOW_SECONDS:
            self.add("warning", "slow-page", url, f"took {response.elapsed:.1f} s to load")
        elif response.elapsed >= SLOW_SECONDS:
            self.add("notice", "slow-page", url, f"took {response.elapsed:.1f} s to load")
        if len(response.body) >= HEAVY_HTML_BYTES:
            self.add("notice", "heavy-page", url, f"HTML is {len(response.body) // 1024} KB")
        if not page.lang:
            self.add("warning", "missing-lang", url, "<html> has no lang attribute")
        if "viewport" not in page.meta:
            self.add("warning", "missing-viewport", url, "no <meta name=viewport>")
        if not page.title:
            self.add("error", "missing-title", url, "page has no <title>")
        elif indexable and not TITLE_RANGE[0] <= len(page.title) <= TITLE_RANGE[1]:
            self.add("notice", "title-length", url,
                     f"title is {len(page.title)} characters (aim for {TITLE_RANGE[0]}-{TITLE_RANGE[1]}): {page.title!r}")
        description = page.meta.get("description", "").strip()
        if indexable and not description:
            self.add("warning", "missing-description", url, "no <meta name=description>")
        elif indexable and not DESCRIPTION_RANGE[0] <= len(description) <= DESCRIPTION_RANGE[1]:
            self.add("notice", "description-length", url,
                     f"description is {len(description)} characters (aim for {DESCRIPTION_RANGE[0]}-{DESCRIPTION_RANGE[1]})")
        h1 = [heading for heading in page.headings if heading["level"] == 1]
        if not h1:
            self.add("warning", "missing-h1", url, "page has no <h1>")
        elif len(h1) > 1:
            self.add("notice", "multiple-h1", url, f"page has {len(h1)} <h1> elements")
        previous = 0
        for heading in page.headings:
            if previous and heading["level"] > previous + 1:
                self.add("notice", "heading-skip", url,
                         f"h{previous} is followed by h{heading['level']} ({heading['text'][:60]!r})")
                break
            previous = heading["level"]
        if indexable and page.canonical is None:
            self.add("notice", "missing-canonical", url, "no <link rel=canonical>")
        elif page.canonical is not None:
            canonical = normalize(page.canonical.strip(), self.pages[url]["final"]) if page.canonical.strip() else None
            if canonical is None:
                self.add("warning", "invalid-canonical", url, f"canonical is not a URL: {page.canonical!r}")
            elif canonical != self.pages[url]["final"]:
                self.pages[url]["canonical"] = canonical
                self.add("notice", "canonical-elsewhere", url, "canonical points at another URL", canonical)
        if indexable:
            missing = [name for name in OG_PROPERTIES if not page.meta.get(name, "").strip()]
            if missing:
                self.add("notice", "missing-social-tags", url, "missing " + ", ".join(missing))
        no_alt = [image.get("src", "") for image in page.images if "alt" not in image]
        if no_alt:
            self.add("warning", "image-missing-alt", url, f"{len(no_alt)} <img> without alt", no_alt[0])
        unsized = [image.get("src", "") for image in page.images
                   if "alt" in image and not (image.get("width") and image.get("height"))]
        if unsized:
            self.add("notice", "image-missing-size", url,
                     f"{len(unsized)} <img> without width and height (layout shift)", unsized[0])
        empty = [anchor["href"] for anchor in page.anchors
                 if not anchor["text"] and not anchor["label"].strip() and not anchor["image_alt"]]
        if empty:
            self.add("warning", "empty-link-text", url, f"{len(empty)} link(s) with no text or label", empty[0])
        generic = [anchor["href"] for anchor in page.anchors if anchor["text"].lower().strip(" .!→>") in GENERIC_LINK_TEXT]
        if generic:
            self.add("notice", "generic-link-text", url, f"{len(generic)} link(s) say only 'here', 'more' or similar",
                     generic[0])
        counts = defaultdict(int)
        for ident in page.ids:
            counts[ident] += 1
        duplicates = sorted(ident for ident, count in counts.items() if count > 1)
        if duplicates:
            self.add("notice", "duplicate-id", url, "repeated id: " + ", ".join(duplicates[:5]))

    def check_status(self, url):
        if not self.is_site(url) and not self.is_first_party(url) and not self.external:
            return None
        response = self.request(url, method="HEAD")
        if response.status in (403, 404, 405, 501) or response.status is None:
            response = self.request(url, method="GET", limit=1024)
        return response

    def check_targets(self):
        targets = []
        for target in set(self.links) | set(self.assets):
            if target in self.responses:
                continue
            if not self.allowed(target) and self.is_site(target):
                continue
            if self.is_site(target) or self.is_first_party(target) or self.external:
                targets.append(target)
        targets.sort()
        with ThreadPoolExecutor(max_workers=self.workers) as pool:
            for index in range(0, len(targets), self.workers * 4):
                if self.out_of_time():
                    self.unchecked += len(targets) - index
                    self.truncated = True
                    break
                chunk = targets[index:index + self.workers * 4]
                for target, response in zip(chunk, pool.map(self.check_status, chunk)):
                    if response is not None:
                        self.responses[target] = response
        for target, sources in sorted(self.links.items()):
            self.judge_target(target, sources, "link")
        for target, sources in sorted(self.assets.items()):
            self.judge_target(target, sources, "asset")
        self.check_fragments()

    def judge_target(self, target, sources, kind):
        response = self.responses.get(target)
        if response is None:
            return
        if response.status == 429:
            self.unchecked += 1
            return
        broken = response.status is None or response.status >= 400
        external = not self.is_first_party(target)
        if broken and (challenged(response) or (external and response.status in UNVERIFIABLE_CODES)):
            self.unverifiable += 1
            return
        if broken:
            status = str(response.status or response.error)
            if response.hops:
                status += f" after redirect to {response.final_url}"
            if external:
                self.add_linked("warning", f"broken-external-{kind}", target, status, sources)
            else:
                self.add_linked("error", f"broken-{kind}", target, status, sources)
        elif response.hops and kind == "link" and self.is_site(target):
            final = normalize(response.final_url)
            if final and self.is_site(final):
                self.add_linked("notice", "link-redirects", target,
                                f"{response.hops[0]['status']} redirect to {final}; link to it directly", sources)

    def check_fragments(self):
        for target, entries in sorted(self.fragments.items()):
            page = self.pages.get(target)
            if page is None:
                page = next((info for info in self.pages.values() if info["final"] == target), None)
            if page is None:
                continue
            ids = set(page["parser"].ids)
            missing = defaultdict(set)
            for fragment, source in entries:
                decoded = urllib.parse.unquote(fragment)
                if decoded in ids or decoded in ("top", "") or decoded.startswith(("!", "/", ":~:")):
                    continue
                missing[decoded].add(source)
            for decoded, sources in sorted(missing.items()):
                self.add_linked("warning", "broken-anchor", f"{target}#{decoded}",
                                f"#{decoded} is not an id on the target page (unless a script adds it)", sources)

    def check_soft_404(self):
        missing = f"{self.origin()}/forkmesh-site-crawler-missing-page"
        response = self.request(missing)
        if response.status == 200:
            self.add("warning", "soft-404", missing, "a page that does not exist answers 200 instead of 404")
        elif response.status == 404 and not response.is_html:
            self.add("notice", "plain-404", missing, "the 404 response is not an HTML page")

    def check_site_wide(self):
        html_pages = {url: info for url, info in self.pages.items()}
        indexable = {url: info for url, info in html_pages.items() if info.get("indexable")}
        for header in SECURITY_HEADERS:
            lacking = sorted(url for url, info in html_pages.items() if header not in info["response"].headers)
            if lacking:
                self.add("warning", "missing-security-header", lacking[0],
                         f"{header} missing on {len(lacking)} of {len(html_pages)} pages", pages=lacking)
        lacking = sorted(
            url for url, info in html_pages.items()
            if "x-frame-options" not in info["response"].headers
            and "frame-ancestors" not in info["response"].headers.get("content-security-policy", "")
        )
        if lacking:
            self.add("warning", "missing-security-header", lacking[0],
                     f"clickjacking protection (x-frame-options or CSP frame-ancestors) missing on {len(lacking)} of {len(html_pages)} pages",
                     pages=lacking)
        for key, check in (("title", "duplicate-title"), ("description", "duplicate-description")):
            groups = defaultdict(list)
            for url, info in indexable.items():
                if info.get("canonical"):
                    continue
                parser = info["parser"]
                value = parser.title if key == "title" else parser.meta.get("description", "").strip()
                if value:
                    groups[value].append(url)
            for value, urls in sorted(groups.items()):
                if len(urls) > 1:
                    urls.sort()
                    self.add("warning", check, urls[0], f"{len(urls)} pages share the {key} {value[:80]!r}", pages=urls)
        sitemap = set(self.sitemap_urls)
        for url in self.sitemap_urls:
            response = self.responses.get(url)
            if response is None or response.status == 429:
                continue
            if response.status is None or response.status >= 400:
                self.add("error", "sitemap-url-error", url, f"listed in the sitemap but answers {response.status or response.error}")
            elif response.hops:
                self.add("warning", "sitemap-url-redirects", url, f"listed in the sitemap but redirects to {response.final_url}")
            elif url in html_pages and not html_pages[url].get("indexable"):
                self.add("warning", "sitemap-url-noindex", url, "listed in the sitemap but marked noindex")
            elif url in html_pages and html_pages[url].get("canonical"):
                self.add("notice", "sitemap-url-not-canonical", url, "listed in the sitemap but its canonical is another URL",
                         html_pages[url]["canonical"])
        if self.sitemap_urls:
            for url, info in sorted(indexable.items()):
                if url not in sitemap and info["final"] not in sitemap and not info.get("canonical") and not info["response"].hops:
                    self.add("notice", "missing-from-sitemap", url, "indexable page is not in the sitemap")
            if not self.truncated and not self.include:
                linked = set(self.linked_from)
                for target in self.linked_from:
                    response = self.responses.get(target)
                    if response is not None and response.hops:
                        linked.add(normalize(response.final_url))
                for url in self.sitemap_urls:
                    if self.is_site(url) and url != self.start and url not in linked and url in html_pages:
                        self.add("notice", "orphan-page", url, "in the sitemap but no crawled page links to it")

    def report(self, elapsed):
        order = {severity: index for index, severity in enumerate(SEVERITIES)}
        self.findings.sort(key=lambda item: (order[item["severity"]], item["check"], -len(item.get("pages", ())),
                                             item["url"], item.get("target", "")))
        counts = {severity: sum(1 for item in self.findings if item["severity"] == severity) for severity in SEVERITIES}
        return {
            "site": self.start,
            "crawledAt": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "stats": {
                "pagesCrawled": self.crawled,
                "htmlPages": len(self.pages),
                "sitemapUrls": len(self.sitemap_urls),
                "linksChecked": len([url for url in set(self.links) | set(self.assets) if url in self.responses]),
                "unverifiable": self.unverifiable,
                "unchecked": self.unchecked,
                "rateLimited": self.rate_limited,
                "truncated": self.truncated,
                "seconds": round(elapsed, 1),
                **counts,
            },
            "findings": self.findings,
        }


def challenged(response):
    return response.headers.get("cf-mitigated", "").lower() == "challenge"


def finding_key(finding):
    if finding.get("target"):
        return (finding["check"], finding["target"])
    return (finding["check"], finding["url"])


def format_text(report, examples=DEFAULT_EXAMPLES):
    stats = report["stats"]
    lines = [
        f"Site crawl of {report['site']} at {report['crawledAt']}",
        f"{stats['htmlPages']} HTML pages, {stats['linksChecked']} links and assets checked, "
        f"{stats['sitemapUrls']} sitemap URLs, {stats['seconds']} s"
        + ("; truncated by the page or time limit" if stats["truncated"] else "")
        + (f"; {stats['unchecked']} URLs not checked" if stats["unchecked"] else "")
        + (f"; rate limited {stats['rateLimited']} times" if stats.get("rateLimited") else ""),
        f"{stats['error']} errors, {stats['warning']} warnings, {stats['notice']} notices"
        + (f"; {stats['unverifiable']} links could not be verified (bot challenge or refused)" if stats["unverifiable"] else ""),
    ]
    for severity in SEVERITIES:
        group = [item for item in report["findings"] if item["severity"] == severity]
        if not group:
            continue
        lines += ["", severity.upper() + "S"]
        by_check = defaultdict(list)
        for item in group:
            by_check[item["check"]].append(item)
        for check, items in sorted(by_check.items(), key=lambda pair: (-len(pair[1]), pair[0])):
            lines.append(f"  {check} ({len(items)})")
            for item in items[:examples]:
                line = f"    {item['url']}"
                if item.get("target"):
                    line += f" -> {item['target']}"
                line += f": {item['detail']}"
                if item.get("source"):
                    line += f"  [{item['source']}]"
                lines.append(line)
            if len(items) > examples:
                lines.append(f"    ... and {len(items) - examples} more")
    return "\n".join(lines)


def compare(old, new):
    before = {finding_key(item): item for item in old["findings"]}
    after = {finding_key(item): item for item in new["findings"]}
    return {
        "new": [after[key] for key in sorted(set(after) - set(before))],
        "resolved": [before[key] for key in sorted(set(before) - set(after))],
        "unchanged": len(set(before) & set(after)),
    }


def format_compare(result):
    lines = [f"{len(result['new'])} new, {len(result['resolved'])} resolved, {result['unchanged']} unchanged"]
    for title, key in (("NEW", "new"), ("RESOLVED", "resolved")):
        if result[key]:
            lines += ["", title]
            for item in result[key]:
                target = f" -> {item['target']}" if item.get("target") else ""
                lines.append(f"  [{item['severity']}] {item['check']}: {item['url']}{target}: {item['detail']}")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Crawl a website (forkmesh.com by default) read-only and report broken links, SEO, "
        "accessibility, performance and security-header issues.",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    crawl = commands.add_parser("crawl", help="crawl the site and report findings")
    crawl.add_argument("--start", default=DEFAULT_START, help="start URL (default %(default)s)")
    crawl.add_argument("--max-pages", type=int, default=DEFAULT_MAX_PAGES)
    crawl.add_argument("--time-budget", type=float, default=DEFAULT_TIME_BUDGET, help="seconds before the crawl stops")
    crawl.add_argument("--workers", type=int, default=DEFAULT_WORKERS)
    crawl.add_argument("--rate", type=float, default=DEFAULT_RATE,
                       help="requests per second across all workers (forkmesh.com allows 12; default %(default)s)")
    crawl.add_argument("--external", action="store_true", help="also check links to other sites")
    crawl.add_argument("--include", action="append", default=[], metavar="PREFIX",
                       help="only crawl pages whose path starts with PREFIX (repeatable), e.g. /docs/")
    crawl.add_argument("--no-sitemap", action="store_true", help="do not seed the crawl from the sitemap")
    crawl.add_argument("--json", action="store_true", help="print the full report as JSON")
    crawl.add_argument("--out", help="also save the JSON report to this file")
    crawl.add_argument("--examples", type=int, default=DEFAULT_EXAMPLES, help="examples per check in the text report")
    crawl.add_argument("--strict", action="store_true", help="exit 1 when there are errors")
    page = commands.add_parser("page", help="audit one page and the links on it")
    page.add_argument("url")
    page.add_argument("--external", action="store_true")
    page.add_argument("--json", action="store_true")
    diff = commands.add_parser("compare", help="compare two saved JSON reports")
    diff.add_argument("old")
    diff.add_argument("new")
    diff.add_argument("--json", action="store_true")
    args = parser.parse_args(argv)

    if args.command == "compare":
        result = compare(json.loads(Path(args.old).read_text(encoding="utf-8")),
                         json.loads(Path(args.new).read_text(encoding="utf-8")))
        print(json.dumps(result, indent=2) if args.json else format_compare(result))
        return 0
    if args.command == "page":
        crawler = Crawler(args.url, external=args.external, use_sitemap=False, follow=False)
        report = crawler.run()
        print(json.dumps(report, indent=2) if args.json else format_text(report, examples=50))
        return 0
    crawler = Crawler(
        args.start,
        max_pages=args.max_pages,
        time_budget=args.time_budget,
        workers=args.workers,
        rate=args.rate,
        external=args.external,
        use_sitemap=not args.no_sitemap,
        include=args.include,
    )
    report = crawler.run()
    if args.out:
        Path(args.out).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2) if args.json else format_text(report, examples=args.examples))
    return 1 if args.strict and report["stats"]["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
