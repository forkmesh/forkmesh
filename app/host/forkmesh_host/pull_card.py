from __future__ import annotations

import asyncio
import html
import io
import re
import time
import urllib.parse

from . import diff_health

CARD_REVISION = "1"
CARD_SECONDS = 600.0
MAX_CARDS = 64
AVATAR_SECONDS = 600.0
MAX_AVATARS = 256
AVATAR_BUDGET_SECONDS = 3.0
AVATAR_SOURCE_SIZE = 128
AVATAR_MAX_BYTES = 256 * 1024
MAX_PEOPLE = 24
QUOTE_CHARS = 160
ACCOUNT_KEY_RE = re.compile(r"^[a-z0-9](?:[a-z0-9-]{0,62})$")
CARD_PATH_RE = re.compile(
    r"^/([A-Za-z0-9][A-Za-z0-9_-]{0,63})/([A-Za-z0-9._-]{1,100})/"
    r"pulls?/([1-9]\d{0,8})\.png$")
SHORT_CARD_PATH_RE = re.compile(
    r"^/([A-Za-z0-9_-][A-Za-z0-9._-]{0,99})/pulls?/([1-9]\d{0,8})\.png$")
COMMENT_EVENTS = ("comment", "line-comment", "thread-comment", "thread-reply")
REVIEW_ORDER = {"changes_requested": 0, "approved": 1, "commented": 2, "ai": 3}
AI_REVIEW_MARK = "Automated AI review"
AI_AUTHORS = ("forkbot",)
MARKDOWN_NOISE = re.compile(r"[`*_>#~]+|!?\[([^\]]*)\]\([^)]*\)")
SECTION_RE = re.compile(r"^#{2,6}\s*(.+?)\s*$", re.M)

_cards = {}
_avatars = {}


class CardTicket:
    def __init__(self, fastpath, owner, repo, number):
        self.fastpath = fastpath
        self.owner = owner
        self.repo = repo
        self.number = number


def card_target(scope):
    if scope.get("method", "GET").upper() not in ("GET", "HEAD"):
        return None
    raw = scope.get("raw_path")
    path = raw.decode("latin-1") if raw else (scope.get("path") or "")
    from .repo_ssr import RESERVED_OWNERS
    match = CARD_PATH_RE.match(path)
    if match:
        owner, repo, number = match.groups()
        if owner.lower() in RESERVED_OWNERS or repo in (".", "..") \
                or repo.endswith(".git"):
            return None
        return owner.lower(), repo, int(number)
    match = SHORT_CARD_PATH_RE.match(path)
    if not match or match.group(1).lower() in RESERVED_OWNERS \
            or match.group(1).endswith(".git"):
        return None
    return None, match.group(1), int(match.group(2))


def _og_card():
    import og_card
    return og_card


def _pull(bundle):
    pull = (bundle.model or {}).get("pull") if bundle is not None else None
    return pull if isinstance(pull, dict) else None


def _clean(text, limit=QUOTE_CHARS):
    text = MARKDOWN_NOISE.sub(lambda m: m.group(1) or " ", str(text or ""))
    text = " ".join(text.split())
    return text if len(text) <= limit else text[:limit - 1].rstrip() + "…"


def _first_paragraph(text):
    for block in re.split(r"\n\s*\n", str(text or "")):
        if block.strip() and not block.lstrip().startswith(("#", ">", "_")):
            return block
    return ""


def ai_summary(body):
    body = str(body or "")
    found = None
    for match in SECTION_RE.finditer(body):
        if found is not None:
            return _clean(_first_paragraph(body[found:match.start()]))
        if match.group(1).strip().lower() == "summary":
            found = match.end()
    return _clean(_first_paragraph(body[found:])) if found is not None else ""


def _key(event):
    return str(event.get("authorKey") or event.get("author") or "").strip()


def _is_ai(event):
    return _key(event).lower() in AI_AUTHORS \
        and AI_REVIEW_MARK in str(event.get("body") or "")


def summary(bundle):
    pull = _pull(bundle)
    if pull is None:
        return None
    from . import repo_views
    events = repo_views.folded_conversation(pull.get("conversation"))
    author_key = str(pull.get("authorKey") or "").strip()
    people = {}

    def meet(key, name):
        key = str(key or name or "").strip()
        if key and key.lower() not in people and len(people) < MAX_PEOPLE:
            people[key.lower()] = {"key": key, "name": str(name or key)}

    meet(author_key, pull.get("author"))
    latest = {}
    policy = []
    comments = 0
    commenters = set()
    ai = None
    for event in events:
        kind = str(event.get("type") or "")
        key = _key(event)
        body = str(event.get("body") or "").strip()
        if kind == "review":
            state = str(event.get("state") or "commented").lower()
            policy.append([key, state])
            if key and key != author_key:
                latest[key.lower()] = {
                    "name": event.get("author") or key, "key": key,
                    "state": state if state in REVIEW_ORDER else "commented",
                    "text": _clean(body)}
            if body:
                comments += 1
                commenters.add(key.lower())
            meet(key, event.get("author"))
        elif kind in COMMENT_EVENTS and body:
            if kind == "comment" and _is_ai(event):
                ai = event
                continue
            comments += 1
            commenters.add(key.lower())
            meet(key, event.get("author"))
    reviews = sorted(latest.values(),
                     key=lambda r: REVIEW_ORDER.get(r["state"], 9))
    if ai is not None:
        reviews.append({"name": ai.get("author") or "ForkBot",
                        "key": _key(ai), "state": "ai",
                        "text": ai_summary(ai.get("body"))})
        meet(_key(ai), ai.get("author") or "ForkBot")
    status = str(pull.get("state") or "open").lower()
    if status == "open" and pull.get("draft"):
        status = "draft"
    info = {
        "number": int(pull.get("number") or bundle.number or 0),
        "title": str(pull.get("title") or ""),
        "state": status,
        "author": str(pull.get("author") or "unknown"),
        "head": str(pull.get("head") or ""),
        "base": str(pull.get("base") or ""),
        "ts": pull.get("ts") or "",
        "commits": int(pull.get("commits") or 0),
        "comments": comments,
        "commenters": len(commenters - {""}),
        "review": repo_views.review_state(policy, author_key, status,
                                          status == "draft"),
        "reviews": reviews,
        "people": list(people.values()),
    }
    patch = str(pull.get("patch") or "")
    if patch.strip():
        files = diff_health.parse_files(patch)
        info["files"] = len(files)
        info["additions"] = sum(f["adds"] for f in files)
        info["deletions"] = sum(f["dels"] for f in files)
        info["health"] = diff_health.report(files)
    return info


def _plural(n, word, many=None):
    return "{:,} {}".format(n, word if n == 1 else many or word + "s")


def description(info):
    parts = []
    verb = {"merged": "merged", "closed": "proposed merging"}.get(
        info["state"], "wants to merge")
    what = _plural(info["commits"], "commit") if info["commits"] else ""
    if info["head"] and info["base"]:
        parts.append("%s %s %s into %s from %s" % (
            info["author"], verb, what or "changes", info["base"],
            info["head"]))
    if info.get("files"):
        parts.append("+{:,} −{:,} in {}".format(
            info["additions"], info["deletions"],
            _plural(info["files"], "file")))
    review = info.get("review") or {}
    if review.get("state") == "approved":
        parts.append("approved by " + _plural(review.get("approvals") or 0,
                                              "reviewer"))
    elif review.get("state") == "changes_requested":
        parts.append("changes requested")
    elif review.get("state") == "review_required":
        parts.append("review required")
    if info["comments"]:
        parts.append("%s from %s" % (
            _plural(info["comments"], "comment"),
            _plural(info["commenters"], "person", "people")))
    if info.get("health"):
        parts.append("code health: " + ", ".join(
            "%s %d" % (m["label"].lower(), m["score"]) for m in info["health"]))
    return " · ".join(parts)


def _base(bundle, link=None):
    return link or "/%s/%s" % (urllib.parse.quote(bundle.owner),
                               urllib.parse.quote(bundle.repo))


def image_url(bundle, origin, link=None):
    return "%s%s/pull/%d.png?v=%s.%s" % (
        origin, _base(bundle, link), bundle.number, bundle.version,
        CARD_REVISION)


def _set_meta(page, attr, name, value):
    pattern = re.compile(r'(<meta %s="%s" content=")[^"]*(">)' % (
        attr, re.escape(name)))
    return pattern.sub(lambda m: m.group(1) + html.escape(value, quote=True)
                       + m.group(2), page, count=1)


def share_tags(page, bundle, origin, link=None):
    pull = _pull(bundle)
    if pull is None or '<meta property="og:image"' not in page or not origin:
        return page
    from .repo_ssr_issue import public_site_origin
    origin = public_site_origin(origin)
    info = summary(bundle)
    shown = (link or "/%s/%s" % (bundle.owner, bundle.repo)).strip("/")
    title = "%s · Pull Request #%d · %s" % (
        info["title"] or "Pull request", info["number"], shown)
    url = "%s%s/pull/%d" % (origin, _base(bundle, link), info["number"])
    text = description(info) or title
    image = image_url(bundle, origin, link)
    for attr, name, value in (
            ("property", "og:url", url), ("property", "og:title", title),
            ("property", "og:description", text),
            ("property", "og:image", image),
            ("property", "og:image:alt", title),
            ("name", "twitter:title", title),
            ("name", "twitter:description", text),
            ("name", "twitter:image", image),
            ("name", "twitter:image:alt", title)):
        page = _set_meta(page, attr, name, value)
    page = re.sub(r'(<link rel="canonical" href=")[^"]*(">)',
                  lambda m: m.group(1) + html.escape(url, quote=True)
                  + m.group(2), page, count=1)
    return re.sub(r"<title>[^<]*</title>",
                  lambda m: "<title>%s</title>" % html.escape(title),
                  page, count=1)


def _decode_avatar(body, sizes):
    try:
        from PIL import Image
    except ImportError:
        return {}
    try:
        with Image.open(io.BytesIO(body)) as source:
            width, height = source.size
            if width <= 0 or height <= 0 or width * height > 1024 * 1024:
                return {}
            image = source.convert("RGBA")
    except Exception:
        return {}
    side = min(width, height)
    image = image.crop(((width - side) // 2, (height - side) // 2,
                        (width - side) // 2 + side,
                        (height - side) // 2 + side))
    return {size: image.resize((size, size), Image.Resampling.LANCZOS)
            .tobytes() for size in sizes}


def _remember(store, key, value, limit):
    if len(store) >= limit:
        store.pop(next(iter(store)))
    store[key] = (time.monotonic(), value)


async def _avatar(worker_fetch, origin, key, sizes):
    key = key.lower()
    hit = _avatars.get(key)
    if hit is not None and time.monotonic() - hit[0] < AVATAR_SECONDS:
        return hit[1]
    from . import js as _js
    parts = urllib.parse.urlsplit(origin)
    headers = _js.Headers({"host": parts.netloc,
                           "user-agent": "forkmesh-host-pull-card",
                           "cf-connecting-ip": "127.0.0.1",
                           "accept": "image/webp,image/png"})
    found = {}
    try:
        response = await worker_fetch(_js.Request(
            "%s/api/accounts/%s/avatar?size=%d" % (
                origin, urllib.parse.quote(key), AVATAR_SOURCE_SIZE),
            "GET", headers))
        kind = str(response.headers.get("content-type") or "")
        if int(response.status) == 200 and kind.startswith(
                ("image/webp", "image/png")):
            body = await response._read()
            if len(body) <= AVATAR_MAX_BYTES:
                found = await asyncio.to_thread(_decode_avatar, body, sizes)
    except Exception as error:
        print("pull card avatar %s skipped: %r" % (key, error))
        return {}
    _remember(_avatars, key, found, MAX_AVATARS)
    return found


async def _with_avatars(info, worker_fetch, origin):
    og_card = _og_card()
    sizes = (og_card.PULL_AVATAR, og_card.PULL_REVIEW_AVATAR)
    wanted = [p for p in info["people"][:og_card.PULL_PEOPLE_SHOWN]]
    shown = {p["key"].lower() for p in wanted}
    wanted += [p for p in info["people"]
               if p["key"].lower() not in shown and any(
                   r.get("key", "").lower() == p["key"].lower()
                   for r in info["reviews"])]
    keys = [p["key"] for p in wanted
            if ACCOUNT_KEY_RE.match(p["key"].lower())
            and p["key"].lower() not in AI_AUTHORS]
    found = {}
    if keys and origin:
        tasks = {key: asyncio.ensure_future(_avatar(worker_fetch, origin, key,
                                                    sizes)) for key in keys}
        done, pending = await asyncio.wait(tasks.values(),
                                           timeout=AVATAR_BUDGET_SECONDS)
        for task in pending:
            task.cancel()
        found = {key.lower(): task.result() for key, task in tasks.items()
                 if task in done and not task.exception()}
    for person in info["people"]:
        person["avatars"] = found.get(person["key"].lower()) or {}
    return info


async def render(bundle, worker_fetch, origin, link=None):
    key = (bundle.owner, bundle.repo, bundle.number, bundle.version, origin,
           link)
    hit = _cards.get(key)
    if hit is not None and time.monotonic() - hit[0] < CARD_SECONDS:
        return hit[1]
    info = summary(bundle)
    if info is None:
        return None
    info = await _with_avatars(info, worker_fetch, origin)
    from .repo_ssr_issue import public_site_origin
    info["host"] = urllib.parse.urlsplit(
        public_site_origin(origin)).netloc or "forkmesh.com"
    info["path"] = (link or "/%s/%s" % (bundle.owner, bundle.repo)).strip("/")
    og_card = _og_card()
    png = await asyncio.to_thread(og_card.render_pull_card, info, time.time())
    _remember(_cards, key, png, MAX_CARDS)
    return png


async def respond(ticket, worker_fetch, request):
    from . import js as _js
    from . import repo_ssr, repo_views
    origin = repo_ssr.request_origin(request)
    owner = ticket.owner or await repo_ssr._short_owner(worker_fetch, request,
                                                        ticket.repo)
    if not owner:
        return None
    bundle = await repo_views.build(ticket.fastpath, owner, ticket.repo,
                                    "pull", ticket.number)
    if _pull(bundle) is None:
        return None
    link = None if ticket.owner else "/" + urllib.parse.quote(ticket.repo)
    png = await render(bundle, worker_fetch, origin, link)
    if not png:
        return None
    return _js.Response(png, 200, "", _js.Headers({
        "content-type": "image/png",
        "cache-control": "public, max-age=300",
        "x-content-type-options": "nosniff",
    }))
