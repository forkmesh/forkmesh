from __future__ import annotations

import datetime
import hashlib
import json
import re

import repository_file_edits as edits

SCHEMA = "forkmesh-forum-event-v1"
FORUM_PATH = ".forkmesh/forum"
PAGE_SIZE = 50
LATEST_PER_CATEGORY = 3
NATIVE_CATEGORY_BASE = 1_000_000
CLOCK_SKEW_MS = 15 * 60 * 1000
MAX_BODY = 32_000
MAX_LOCAL_POSTS = 2000
MAX_LIKERS = 5000
MAX_ATTEMPTS = 3
MAX_FILE_BYTES = 1_500_000
MAX_WRITES_PER_ACCOUNT_PER_HOUR = 240
TOPIC_RE = re.compile(r"^(?:[1-9]\d{0,9}|n[1-9]\d{0,8})$")
POST_RE = re.compile(r"^(?:[1-9]\d{0,6}|l[1-9]\d{0,6})$")
EVENT_ID_RE = re.compile(r"^[a-z][a-z-]{1,20}-[0-9a-z-]{4,60}$")
PUBKEY_RE = re.compile(r"^[A-Za-z0-9_-]{43}$")
SIG_RE = re.compile(r"^[A-Za-z0-9_-]{86}$")
NAME_RE = re.compile(r"^[a-z0-9][a-z0-9_.-]{0,38}$")
COLOR_RE = re.compile(r"^(?:[0-9a-fA-F]{3}|[0-9a-fA-F]{6})$")
TAG_RE = re.compile(r"^[^\s/\\<>\"'`]{1,40}$")

AUTHOR_TYPES = frozenset({"topic", "post", "edit", "delete", "like", "unlike"})
STATE_TYPES = {
    "pin": ("pinned", True), "unpin": ("pinned", False),
    "close": ("closed", True), "open": ("closed", False),
    "archive": ("archived", True), "unarchive": ("archived", False),
    "list": ("visible", True), "unlist": ("visible", False),
}
OPERATOR_TYPES = frozenset(set(STATE_TYPES) | {
    "move", "hide", "unhide", "category", "category-delete", "site", "silence", "unsilence"})
ALL_TYPES = AUTHOR_TYPES | OPERATOR_TYPES
TOPIC_LESS = frozenset({"topic", "category", "category-delete", "site", "silence", "unsilence"})
POST_LEVEL = frozenset({"edit", "delete", "like", "unlike", "hide", "unhide"})


class RecordError(Exception):
    def __init__(self, code, message, status=400):
        super().__init__(message)
        self.code = code
        self.message = message
        self.status = status


def _bind_runtime(runtime):
    edits._bind_runtime(runtime)
    namespace = globals()
    for name, value in runtime.items():
        if not name.startswith("__") and name not in namespace:
            namespace[name] = value


def content_text(content):
    return json.dumps(content, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def canonical(repo_key, event):
    digest = hashlib.sha256(content_text(event["content"]).encode("utf-8")).hexdigest()
    return "\n".join([SCHEMA, repo_key, event["type"], event["topic"], event["post"],
                      event["author"], str(event["ts"]), digest])


def _text(value, low, high, field):
    if not isinstance(value, str) or not low <= len(value) <= high or "\x00" in value:
        raise RecordError("invalid_content", "%s is missing or too long." % field)
    return value


def _int(value, low, high, field):
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise RecordError("invalid_content", "%s is not a valid number." % field)
    return value


def _check_content(kind, content):
    if not isinstance(content, dict):
        raise RecordError("invalid_content", "The event has no content.")
    allowed = {
        "topic": {"title", "body", "category", "tags"},
        "post": {"body", "replyTo"},
        "edit": {"body", "title"},
        "pin": {"globally"},
        "move": {"category"},
        "category": {"id", "name", "color", "description", "parent", "position"},
        "category-delete": {"id"},
        "site": {"title", "description"},
        "silence": {"account", "days"},
        "unsilence": {"account"},
    }.get(kind, set())
    if set(content) - allowed:
        raise RecordError("invalid_content", "The event has fields it should not.")
    if kind == "topic":
        _text(content.get("title"), 1, 255, "The title")
        _text(content.get("body"), 1, MAX_BODY, "The post")
        _int(content.get("category"), 0, 10 ** 10, "The category")
        tags = content.get("tags", [])
        if not isinstance(tags, list) or len(tags) > 5 or not all(
                isinstance(tag, str) and TAG_RE.match(tag) for tag in tags):
            raise RecordError("invalid_content", "Tags are short words, at most five.")
    elif kind == "post":
        _text(content.get("body"), 1, MAX_BODY, "The post")
        reply = content.get("replyTo", "")
        if not isinstance(reply, str) or (reply and not POST_RE.match(reply)):
            raise RecordError("invalid_content", "The post replied to is not valid.")
    elif kind == "edit":
        if "body" not in content and "title" not in content:
            raise RecordError("invalid_content", "An edit changes the post or the title.")
        if "body" in content:
            _text(content["body"], 1, MAX_BODY, "The post")
        if "title" in content:
            _text(content["title"], 1, 255, "The title")
    elif kind == "pin":
        if not isinstance(content.get("globally", False), bool):
            raise RecordError("invalid_content", "globally is true or false.")
    elif kind == "move":
        _int(content.get("category"), 1, 10 ** 10, "The category")
    elif kind == "category":
        _int(content.get("id"), 0, 10 ** 10, "The category")
        _text(content.get("name"), 1, 50, "The name")
        if not isinstance(content.get("color", ""), str) or (
                content.get("color") and not COLOR_RE.match(content["color"])):
            raise RecordError("invalid_content", "The colour is a hex colour such as 0088CC.")
        _text(content.get("description", ""), 0, 500, "The description")
        _int(content.get("parent", 0), 0, 10 ** 10, "The parent")
        _int(content.get("position", 0), 0, 10_000, "The position")
    elif kind == "category-delete":
        _int(content.get("id"), 1, 10 ** 10, "The category")
    elif kind == "site":
        _text(content.get("title"), 1, 100, "The title")
        _text(content.get("description", ""), 0, 500, "The description")
    elif kind in ("silence", "unsilence"):
        if not isinstance(content.get("account"), str) or not NAME_RE.match(content["account"]):
            raise RecordError("invalid_content", "The account name is not valid.")
        if kind == "silence":
            _int(content.get("days"), 1, 3650, "The number of days")


def clean_event(raw, now_ms):
    if not isinstance(raw, dict):
        raise RecordError("invalid_event", "The event was not understood.")
    kind = raw.get("type")
    if kind not in ALL_TYPES:
        raise RecordError("invalid_event", "Unknown forum action.")
    topic = raw.get("topic", "")
    post = raw.get("post", "")
    if not isinstance(topic, str) or not isinstance(post, str):
        raise RecordError("invalid_event", "The topic or post is not valid.")
    if kind in TOPIC_LESS:
        if topic or post:
            raise RecordError("invalid_event", "This action names no topic.")
    elif not TOPIC_RE.match(topic):
        raise RecordError("invalid_event", "The topic is not valid.")
    if kind in POST_LEVEL:
        if not POST_RE.match(post):
            raise RecordError("invalid_event", "The post is not valid.")
    elif kind not in TOPIC_LESS and post:
        raise RecordError("invalid_event", "This action names no post.")
    ts = raw.get("ts")
    if isinstance(ts, bool) or not isinstance(ts, int) or abs(ts - now_ms) > CLOCK_SKEW_MS:
        raise RecordError("stale_event", "The event's time is off; check this device's clock.")
    for key, pattern in (("author", PUBKEY_RE), ("sig", SIG_RE), ("id", EVENT_ID_RE)):
        if not isinstance(raw.get(key), str) or not pattern.match(raw[key]):
            raise RecordError("invalid_event", "The event is not signed correctly.")
    _check_content(kind, raw.get("content"))
    return {"type": kind, "topic": topic, "post": post, "author": raw["author"], "ts": ts,
            "id": raw["id"], "sig": raw["sig"], "content": raw["content"]}


def iso(ms):
    moment = datetime.datetime.fromtimestamp(ms / 1000, tz=datetime.timezone.utc)
    return moment.strftime("%Y-%m-%dT%H:%M:%S.") + "%03dZ" % (ms % 1000)


def slug(title):
    text = re.sub(r"[^\w]+", "-", str(title or "").lower()).strip("-")[:80]
    return text or "topic"


def merge_row(row, state):
    merged = dict(row)
    state = state or {}
    for key in ("pinned", "pinned_globally", "closed", "archived", "visible", "category_id", "title"):
        if key in state:
            merged[key] = state[key]
    local = int(state.get("local_posts") or 0)
    if local:
        merged["posts_count"] = int(merged.get("posts_count") or 0) + local
        merged["reply_count"] = int(merged.get("reply_count") or 0) + local
    if str(state.get("bumped_at") or "") > str(merged.get("bumped_at") or ""):
        merged["bumped_at"] = state["bumped_at"]
        merged["last_posted_at"] = state["bumped_at"]
        merged["last_poster_username"] = state.get("last_poster_username") or merged.get("last_poster_username", "")
    posters = list(merged.get("posters") or [])
    for poster in state.get("posters") or []:
        if all(item.get("username") != poster.get("username") for item in posters):
            posters.append(poster)
    merged["posters"] = posters[:5]
    return merged


def patch_rows(rows, row, pin_key, remove=False):
    rows = [item for item in rows or [] if str(item.get("id")) != str(row.get("id"))]
    if remove or row.get("visible") is False or row.get("deleted"):
        return rows
    if row.get(pin_key):
        return [row] + rows
    at = 0
    while at < len(rows) and rows[at].get(pin_key):
        at += 1
    return rows[:at] + [row] + rows[at:]


def patch_page(page, row, pin_key, remove=False):
    page = dict(page or {"page": 1, "pages": 1, "more": False, "rows": []})
    rows = patch_rows(page.get("rows"), row, pin_key, remove)
    if len(rows) > PAGE_SIZE:
        rows = rows[:PAGE_SIZE]
        page["more"] = True
    page["rows"] = rows
    return page


def latest_row(row):
    return {key: row.get(key) for key in ("id", "title", "slug", "bumped_at", "last_posted_at",
                                          "posts_count", "pinned", "closed", "archived")}


def patch_categories(categories, row, created=False, remove=False):
    categories = [dict(item) for item in categories or []]
    by_id = {item.get("id"): item for item in categories}
    target = by_id.get(row.get("category_id"))
    family = [target] if target else []
    if target and target.get("parent_category_id") in by_id:
        family.append(by_id[target["parent_category_id"]])
    for index, category in enumerate(family):
        latest = [item for item in category.get("latest") or [] if str(item.get("id")) != str(row.get("id"))]
        if not remove and row.get("visible") is not False and not row.get("deleted"):
            latest = [latest_row(row)] + latest
        category["latest"] = latest[:LATEST_PER_CATEGORY]
        if created:
            if index == 0:
                category["topic_count"] = int(category.get("topic_count") or 0) + 1
            category["total_topic_count"] = int(category.get("total_topic_count") or 0) + 1
    return categories


def link_subcategories(categories):
    ids = {item.get("id") for item in categories}
    for item in categories:
        item["subcategory_ids"] = [child["id"] for child in sorted(
            (child for child in categories if child.get("parent_category_id") == item.get("id")),
            key=lambda child: (child.get("position", 0), child["id"]))]
        if item.get("parent_category_id") not in ids:
            item["parent_category_id"] = None
    return sorted(categories, key=lambda item: (item.get("position", 0), item["id"]))


def _bumped(row):
    return (str(row.get("bumped_at") or ""), str(row.get("id")))


def pages(rows, pin_key):
    ordered = sorted(rows, key=_bumped, reverse=True)
    ordered = [row for row in ordered if row.get(pin_key)] + [row for row in ordered if not row.get(pin_key)]
    count = max(1, -(-len(ordered) // PAGE_SIZE))
    return [{"page": number, "pages": count, "more": number < count,
             "rows": ordered[(number - 1) * PAGE_SIZE:number * PAGE_SIZE]} for number in range(1, count + 1)]


def tag_key(tag):
    if re.fullmatch(r"[\w.-]{1,80}", tag) and not tag.startswith("."):
        return tag
    return "h-" + hashlib.sha256(tag.encode("utf-8")).hexdigest()[:16]


def rebuild_lists(rows, categories):
    listed = [row for row in rows if row.get("visible") is not False and not row.get("deleted")]
    files = {}
    for number, page in enumerate(pages(listed, "pinned_globally"), 1):
        files["latest/%d.json" % number] = page
    categories = link_subcategories([dict(item) for item in categories])
    for category in categories:
        family = {category["id"], *category.get("subcategory_ids", [])}
        members = [row for row in listed if row.get("category_id") in family]
        own = [row for row in listed if row.get("category_id") == category["id"]]
        for number, page in enumerate(pages(members, "pinned"), 1):
            files["c/%s/%d.json" % (category["id"], number)] = page
        category.update(topic_count=len(own), total_topic_count=len(members),
                        post_count=sum(int(row.get("posts_count") or 0) for row in own),
                        latest=[latest_row(row) for row in sorted(members, key=_bumped, reverse=True)[:LATEST_PER_CATEGORY]])
    files["categories.json"] = categories
    tags = {}
    for row in listed:
        for tag in row.get("tags") or []:
            tags[tag] = tags.get(tag, 0) + 1
    tag_list = sorted(({"name": name, "key": tag_key(name), "count": count} for name, count in tags.items()),
                      key=lambda item: (-item["count"], item["name"]))
    files["tags.json"] = tag_list
    for tag in tag_list:
        for number, page in enumerate(pages([row for row in listed if tag["name"] in (row.get("tags") or [])], "pinned"), 1):
            files["tag/%s/%d.json" % (tag["key"], number)] = page
    return files


def new_forum(title):
    return {"format": 1, "kind": "forkmesh-forum", "generator": "ForkMesh forum", "title": title,
            "description": "", "source": {"type": "forkmesh", "origin": "", "lastSync": "", "pending": 0},
            "counts": {"topics": 0, "posts": 0, "categories": 1}, "pageSize": PAGE_SIZE,
            "postsPerChunk": 100, "topPeriods": ["all", "yearly", "quarterly", "monthly", "weekly", "daily"]}


DEFAULT_CATEGORY = {"id": NATIVE_CATEGORY_BASE, "name": "General", "slug": "general", "color": "0088CC",
                    "text_color": "FFFFFF", "description": "", "parent_category_id": None, "position": 0,
                    "subcategory_ids": [], "topic_count": 0, "total_topic_count": 0, "post_count": 0,
                    "latest": [], "origin": "forkmesh"}


class Forum:
    def __init__(self, read, event, ctx):
        self.read = read
        self.event = event
        self.context = ctx
        self.changes = {}
        self.result = {}

    async def get(self, path):
        if path in self.changes:
            return self.changes[path]
        return await self.read(path)

    def put(self, path, value):
        self.changes[path] = value

    async def forum(self):
        forum = await self.get("forum.json")
        if not isinstance(forum, dict):
            forum = new_forum(self.context.get("repoTitle") or "Forum")
            self.put("forum.json", forum)
            self.put("categories.json", [dict(DEFAULT_CATEGORY)])
        return forum

    async def categories(self):
        await self.forum()
        categories = await self.get("categories.json")
        return [item for item in categories or [] if isinstance(item, dict) and item.get("id")]

    async def silenced(self, account):
        moderation = await self.get("moderation.json") or {}
        entry = (moderation.get("silenced") or {}).get(account)
        return bool(entry) and int(entry.get("until") or 0) > self.context["now"]

    async def topic_files(self, tid):
        base = "topics/%s/" % tid
        header = await self.get(base + "topic.json")
        if not isinstance(header, dict):
            raise RecordError("topic_not_found", "That topic is not in the forum.", 404)
        local = await self.get(base + "local.json") or {"posts": [], "next": 1}
        state = await self.get(base + "state.json") or {}
        return base, header, local, state

    async def row(self, tid, header, state):
        if tid.startswith("n"):
            native = await self.get("native.json") or {}
            row = dict((native.get("topics") or {}).get(tid) or {})
            if not row:
                raise RecordError("topic_not_found", "That topic is not in the forum.", 404)
            return row
        row = {key: header.get(key) for key in (
            "id", "title", "slug", "category_id", "tags", "created_at", "bumped_at", "last_posted_at",
            "posts_count", "reply_count", "views", "like_count", "pinned", "pinned_globally", "closed",
            "archived", "visible", "excerpt", "posters", "last_poster_username")}
        row["origin"] = "discourse"
        return merge_row(row, state)

    async def save_row(self, tid, row, old_category=None, created=False, remove=False):
        if tid.startswith("n"):
            native = dict(await self.get("native.json") or {"next": 1, "topics": {}})
            topics = dict(native.get("topics") or {})
            topics[tid] = row
            native["topics"] = topics
            self.put("native.json", native)
        hidden = remove or row.get("visible") is False or row.get("deleted")
        forum = await self.forum()
        if (forum.get("source") or {}).get("type") == "forkmesh":
            native = await self.get("native.json") or {}
            for path, value in rebuild_lists(list((native.get("topics") or {}).values()), await self.categories()).items():
                self.put(path, value)
            return
        self.put("latest/1.json", patch_page(await self.get("latest/1.json"), row, "pinned_globally", hidden))
        categories = await self.categories()
        by_id = {item["id"]: item for item in categories}
        touched = []
        for category_id in (old_category, row.get("category_id")):
            if category_id in by_id and category_id not in touched:
                touched.append(category_id)
                parent = by_id[category_id].get("parent_category_id")
                if parent in by_id and parent not in touched:
                    touched.append(parent)
        new_family = {row.get("category_id"), by_id.get(row.get("category_id"), {}).get("parent_category_id")}
        for category_id in touched:
            path = "c/%s/1.json" % category_id
            self.put(path, patch_page(await self.get(path), row, "pinned",
                                      hidden or category_id not in new_family))
        if old_category is not None and old_category != row.get("category_id"):
            categories = patch_categories(categories, dict(row, category_id=old_category), remove=True)
        self.put("categories.json", patch_categories(categories, row, created=created, remove=hidden))

    async def bump_counts(self, topics=0, posts=0):
        forum = dict(await self.forum())
        counts = dict(forum.get("counts") or {})
        counts["topics"] = max(0, int(counts.get("topics") or 0) + topics)
        counts["posts"] = max(0, int(counts.get("posts") or 0) + posts)
        forum["counts"] = counts
        self.put("forum.json", forum)

    def post_record(self, key, body, reply_to=""):
        now = iso(self.context["now"])
        return {"key": key, "post_number": 0, "post_type": 1, "username": self.context["account"],
                "name": "", "author": self.event["author"], "created_at": now, "updated_at": now,
                "reply_to": reply_to, "format": "markdown", "raw": body, "origin": "forkmesh",
                "event": self.event["id"]}

    def poster(self):
        return {"username": self.context["account"], "name": "", "description": "Poster"}

    async def apply(self):
        kind = self.event["type"]
        if kind in OPERATOR_TYPES and not self.context.get("moderator"):
            raise RecordError("not_moderator", "Only the forum's operators can do that.", 403)
        if kind in AUTHOR_TYPES and not self.context.get("moderator") and await self.silenced(self.context["account"]):
            raise RecordError("silenced", "An operator has silenced you in this forum for now.", 403)
        handler = {
            "topic": self.create_topic, "post": self.create_post, "edit": self.edit_post,
            "delete": self.delete_post, "like": self.like, "unlike": self.like, "move": self.move,
            "hide": self.hide, "unhide": self.hide, "category": self.category,
            "category-delete": self.delete_category, "site": self.site, "silence": self.silence,
            "unsilence": self.silence,
        }.get(kind, self.set_state)
        signed = hashlib.sha256(canonical("", self.event).encode("utf-8")).hexdigest()[:24]
        path = "events/%s/%d-%s.json" % (iso(self.event["ts"])[:7], self.event["ts"], signed)
        if await self.get(path) is not None:
            raise RecordError("duplicate", "This forum event was already saved.", 409)
        await handler()
        self.put(path, dict(self.event, authorName=self.context["account"], receivedAt=self.context["now"]))
        return self.changes, self.result

    async def create_topic(self):
        content = self.event["content"]
        categories = await self.categories()
        category = content.get("category") or 0
        if category not in {item["id"] for item in categories}:
            if category or not categories:
                raise RecordError("category_not_found", "Choose one of the forum's categories.")
            category = categories[0]["id"]
        native = dict(await self.get("native.json") or {"next": 1, "topics": {}})
        number = max(1, int(native.get("next") or 1))
        tid = "n%d" % number
        native["next"] = number + 1
        self.put("native.json", native)
        now = iso(self.context["now"])
        row = {"id": tid, "title": content["title"], "slug": slug(content["title"]), "category_id": category,
               "tags": list(content.get("tags") or []), "created_at": now, "bumped_at": now,
               "last_posted_at": now, "posts_count": 1, "reply_count": 0, "views": 0, "like_count": 0,
               "pinned": False, "pinned_globally": False, "closed": False, "archived": False, "visible": True,
               "excerpt": content["body"][:300], "posters": [dict(self.poster(), description="Original Poster")],
               "last_poster_username": self.context["account"], "origin": "forkmesh", "author": self.event["author"]}
        base = "topics/%s/" % tid
        self.put(base + "topic.json", dict(row, origin="forkmesh", source="", chunks=1, uploads={}, posts=[],
                                           highest_post_number=0, participant_count=1, link_count=0))
        self.put(base + "local.json", {"posts": [self.post_record("l1", content["body"])], "next": 2})
        await self.save_row(tid, row, created=True)
        await self.bump_counts(topics=1, posts=1)
        self.result.update(topic=tid, post="l1")

    async def create_post(self):
        tid = self.event["topic"]
        base, header, local, state = await self.topic_files(tid)
        row = await self.row(tid, header, state)
        if (row.get("closed") or row.get("archived")) and not self.context.get("moderator"):
            raise RecordError("topic_closed", "This topic is closed for new replies.", 403)
        posts = list(local.get("posts") or [])
        if len(posts) >= MAX_LOCAL_POSTS:
            raise RecordError("topic_full", "This topic has too many replies; start a new topic.", 409)
        number = max(1, int(local.get("next") or 1))
        key = "l%d" % number
        posts.append(self.post_record(key, self.event["content"]["body"], self.event["content"].get("replyTo", "")))
        self.put(base + "local.json", {"posts": posts, "next": number + 1})
        now = iso(self.context["now"])
        if tid.startswith("n"):
            row.update(posts_count=int(row.get("posts_count") or 0) + 1,
                       reply_count=int(row.get("reply_count") or 0) + 1, bumped_at=now,
                       last_posted_at=now, last_poster_username=self.context["account"])
            posters = list(row.get("posters") or [])
            if all(item.get("username") != self.context["account"] for item in posters):
                posters.append(self.poster())
            row["posters"] = posters[:5]
        else:
            state = dict(state)
            state["local_posts"] = int(state.get("local_posts") or 0) + 1
            state["bumped_at"] = now
            state["last_poster_username"] = self.context["account"]
            posters = list(state.get("posters") or [])
            if all(item.get("username") != self.context["account"] for item in posters):
                posters.append(self.poster())
            state["posters"] = posters[:5]
            self.put(base + "state.json", state)
            row.update(bumped_at=now, last_posted_at=now, last_poster_username=self.context["account"],
                       posts_count=int(row.get("posts_count") or 0) + 1,
                       reply_count=int(row.get("reply_count") or 0) + 1)
        await self.save_row(tid, row)
        await self.bump_counts(posts=1)
        self.result.update(topic=tid, post=key)

    async def local_post(self, tid, key):
        base, header, local, state = await self.topic_files(tid)
        posts = [dict(item) for item in local.get("posts") or []]
        for item in posts:
            if item.get("key") == key:
                return base, header, local, state, posts, item
        raise RecordError("post_not_found", "That post is not in the topic.", 404)

    def may_change(self, post):
        return self.context.get("moderator") or post.get("author") == self.event["author"]

    async def edit_post(self):
        tid, key = self.event["topic"], self.event["post"]
        if not key.startswith("l"):
            raise RecordError("not_editable", "Posts copied from the original forum are edited there.", 403)
        base, header, local, state, posts, post = await self.local_post(tid, key)
        if not self.may_change(post) or post.get("deleted"):
            raise RecordError("not_yours", "You can only edit your own posts.", 403)
        content = self.event["content"]
        if "body" in content:
            post.update(raw=content["body"], updated_at=iso(self.context["now"]))
        self.put(base + "local.json", dict(local, posts=posts))
        if "title" in content:
            if not tid.startswith("n") or key != "l1":
                raise RecordError("not_editable", "Only the first post of a topic carries its title.", 403)
            row = await self.row(tid, header, state)
            row.update(title=content["title"], slug=slug(content["title"]))
            self.put(base + "topic.json", dict(header, title=row["title"], slug=row["slug"]))
            await self.save_row(tid, row)
        self.result.update(topic=tid, post=key)

    async def delete_post(self):
        tid, key = self.event["topic"], self.event["post"]
        if key.startswith("l"):
            base, header, local, state, posts, post = await self.local_post(tid, key)
            if not self.may_change(post):
                raise RecordError("not_yours", "You can only delete your own posts.", 403)
            post.update(deleted=True, raw="", updated_at=iso(self.context["now"]), deleted_by=self.context["account"])
            self.put(base + "local.json", dict(local, posts=posts))
            if tid.startswith("n") and key == "l1":
                row = await self.row(tid, header, state)
                row["deleted"] = True
                await self.save_row(tid, row, remove=True)
                await self.bump_counts(topics=-1)
        else:
            if not self.context.get("moderator"):
                raise RecordError("not_moderator", "Only the forum's operators can remove copied posts.", 403)
            base, header, local, state = await self.topic_files(tid)
            deleted = sorted(set(state.get("deleted") or []) | {key})
            self.put(base + "state.json", dict(state, deleted=deleted))
        self.result.update(topic=tid, post=key)

    async def like(self):
        tid, key = self.event["topic"], self.event["post"]
        base, header, local, state = await self.topic_files(tid)
        if key.startswith("l"):
            _base, _h, _l, _s, _posts, post = await self.local_post(tid, key)
            if post.get("author") == self.event["author"] or post.get("username") == self.context["account"]:
                raise RecordError("own_post", "You cannot like your own post.", 403)
        likes = dict(await self.get(base + "likes.json") or {})
        names = [name for name in likes.get(key) or [] if name != self.context["account"]]
        if self.event["type"] == "like":
            names.append(self.context["account"])
        if names:
            likes[key] = sorted(names)[:MAX_LIKERS]
        else:
            likes.pop(key, None)
        self.put(base + "likes.json", likes)
        self.result.update(topic=tid, post=key, likes=len(names))

    async def set_state(self):
        tid = self.event["topic"]
        base, header, local, state = await self.topic_files(tid)
        field, value = STATE_TYPES[self.event["type"]]
        updates = {field: value}
        if self.event["type"] == "pin":
            updates["pinned_globally"] = bool(self.event["content"].get("globally"))
        elif self.event["type"] == "unpin":
            updates["pinned_globally"] = False
        row = await self.row(tid, header, state)
        row.update(updates)
        if tid.startswith("n"):
            self.put(base + "topic.json", dict(header, **updates))
        else:
            self.put(base + "state.json", dict(state, **updates))
        await self.save_row(tid, row)
        self.result.update(topic=tid, state=updates)

    async def move(self):
        tid = self.event["topic"]
        category = self.event["content"]["category"]
        if category not in {item["id"] for item in await self.categories()}:
            raise RecordError("category_not_found", "That category is not in the forum.", 404)
        base, header, local, state = await self.topic_files(tid)
        row = await self.row(tid, header, state)
        old = row.get("category_id")
        row["category_id"] = category
        if tid.startswith("n"):
            self.put(base + "topic.json", dict(header, category_id=category))
        else:
            self.put(base + "state.json", dict(state, category_id=category))
        await self.save_row(tid, row, old_category=old)
        self.result.update(topic=tid, category=category)

    async def hide(self):
        tid, key = self.event["topic"], self.event["post"]
        base, header, local, state = await self.topic_files(tid)
        hidden = set(state.get("hidden") or [])
        if self.event["type"] == "hide":
            hidden.add(key)
        else:
            hidden.discard(key)
        self.put(base + "state.json", dict(state, hidden=sorted(hidden)))
        self.result.update(topic=tid, post=key)

    async def category(self):
        content = self.event["content"]
        categories = await self.categories()
        by_id = {item["id"]: item for item in categories}
        parent = content.get("parent") or None
        if parent is not None and (parent not in by_id or by_id[parent].get("parent_category_id")):
            raise RecordError("category_not_found", "The parent must be a top-level category.", 404)
        category_id = content.get("id") or 0
        if category_id and category_id not in by_id:
            raise RecordError("category_not_found", "That category is not in the forum.", 404)
        if not category_id:
            native_ids = [item["id"] for item in categories if item["id"] >= NATIVE_CATEGORY_BASE]
            category_id = max(native_ids + [NATIVE_CATEGORY_BASE - 1]) + 1
        if parent == category_id or (parent and any(
                item.get("parent_category_id") == category_id for item in categories)):
            raise RecordError("invalid_content", "A category with subcategories cannot become one.")
        current = dict(by_id.get(category_id) or {
            "id": category_id, "topic_count": 0, "total_topic_count": 0, "post_count": 0, "latest": [],
            "subcategory_ids": [], "text_color": "FFFFFF", "origin": "forkmesh"})
        current.update(name=content["name"], slug=slug(content["name"]),
                       color=(content.get("color") or current.get("color") or "0088CC").upper(),
                       description=content.get("description", ""), parent_category_id=parent,
                       position=content.get("position", current.get("position", 0)))
        by_id[category_id] = current
        self.put("categories.json", link_subcategories(list(by_id.values())))
        local = dict(await self.get("local.json") or {})
        overrides = dict(local.get("categories") or {})
        overrides[str(category_id)] = {key: current[key] for key in (
            "name", "slug", "color", "description", "parent_category_id", "position")}
        local["categories"] = overrides
        self.put("local.json", local)
        self.result.update(category=category_id)

    async def delete_category(self):
        category_id = self.event["content"]["id"]
        categories = await self.categories()
        target = next((item for item in categories if item["id"] == category_id), None)
        if not target:
            raise RecordError("category_not_found", "That category is not in the forum.", 404)
        if target.get("total_topic_count") or target.get("subcategory_ids") or category_id < NATIVE_CATEGORY_BASE:
            raise RecordError("category_in_use", "Only an empty category made on ForkMesh can be deleted.", 409)
        self.put("categories.json", link_subcategories([item for item in categories if item["id"] != category_id]))
        local = dict(await self.get("local.json") or {})
        overrides = dict(local.get("categories") or {})
        overrides.pop(str(category_id), None)
        local["categories"] = overrides
        self.put("local.json", local)
        self.result.update(category=category_id)

    async def site(self):
        content = self.event["content"]
        forum = dict(await self.forum())
        forum.update(title=content["title"], description=content.get("description", ""))
        self.put("forum.json", forum)
        local = dict(await self.get("local.json") or {})
        local.update(title=content["title"], description=content.get("description", ""))
        self.put("local.json", local)

    async def silence(self):
        content = self.event["content"]
        moderation = dict(await self.get("moderation.json") or {})
        silenced = dict(moderation.get("silenced") or {})
        if self.event["type"] == "silence":
            silenced[content["account"]] = {"until": self.context["now"] + content["days"] * 86_400_000,
                                            "by": self.context["account"]}
        else:
            silenced.pop(content["account"], None)
        moderation["silenced"] = silenced
        self.put("moderation.json", moderation)
        self.result.update(account=content["account"])


async def apply_event(read, event, ctx):
    return await Forum(read, event, ctx).apply()


class Conflict(Exception):
    pass


def encode(value):
    return (json.dumps(value, ensure_ascii=False, indent=1, sort_keys=True) + "\n").encode("utf-8")


async def read_forum(env, owner, repo, branch):
    root = await edits._tree(env, owner, repo, "", branch)
    commit = str((root or {}).get("commit") or "").lower()
    if not edits.OID_RE.match(commit):
        raise RecordError("repository_unavailable", "The repository could not be read right now.", 503)
    cache = {}

    async def read(path):
        if path not in cache:
            blob = await edits._blob(env, owner, repo, FORUM_PATH + "/" + path, commit)
            value = None
            if isinstance(blob, dict) and isinstance(blob.get("content"), str):
                if blob.get("truncated") or str(blob.get("encoding") or "utf8") != "utf8":
                    raise RecordError("forum_unreadable", "Part of the forum is too large to change here.", 409)
                try:
                    value = json.loads(blob["content"])
                except ValueError as error:
                    raise RecordError("forum_unreadable", "Part of the forum could not be read.", 409) from error
            cache[path] = value
        return cache[path]

    return commit, read


def check_sizes(changes):
    for path, value in changes.items():
        if len(encode(value)) > MAX_FILE_BYTES:
            raise RecordError("forum_full", "This part of the forum is full; start a new topic.", 409)


async def forum_listings(env, owner, repo, commit, paths):
    listings = {}
    for path in sorted(paths):
        parts = path.split("/")
        for depth in range(len(parts)):
            directory = "/".join(parts[:depth])
            if directory in listings:
                continue
            parent = "/".join(parts[:depth - 1]) if depth else None
            if parent is not None and not any(
                    entry.get("name") == parts[depth - 1] for entry in listings.get(parent) or []):
                listings[directory] = []
                continue
            listing = await edits._tree(env, owner, repo, directory, commit)
            if listing is None or str(listing.get("commit") or commit).lower() != commit:
                raise Conflict()
            if listing.get("truncated"):
                raise RecordError("forum_unreadable", "A folder of the forum is too large to change here.", 409)
            listings[directory] = listing["entries"]
    return listings


async def commit_records(env, owner, repo, branch, commit, changes, account, message):
    check_sizes(changes)
    files = {FORUM_PATH + "/" + path: encode(value) for path, value in changes.items()}
    listings = await forum_listings(env, owner, repo, commit, files)
    objects = []
    tree_changes = {}
    for path, blob in files.items():
        objects.append(("blob", blob))
        tree_changes[path] = ("100644", edits.object_id("blob", blob))
    root, trees = edits.apply_tree_changes(listings, tree_changes)
    email = edits.noreply_email(account, _public_base_url(env))
    payload = edits.commit_payload(root, commit, account, email, int(Date.now() // 1000), message)
    new_commit = edits.object_id("commit", payload)
    body = edits.receive_pack_body(commit, new_commit, "refs/heads/" + branch,
                                   edits.pack(objects + trees + [("commit", payload)]))
    upstream, _endpoint = await _https_mirror_push_send(
        env, None, owner, repo, account, "POST", body, None, hashlib.sha256(body).hexdigest(),
        alias_owner="", core_held=owner.lower() in _core_push_nodes(env), credential="forum-record")
    if upstream is None:
        raise RecordError("push_unavailable", "No repository host accepted the post; try again.", 503)
    status = int(getattr(upstream, "status", 0) or 0)
    try:
        text = str(await upstream.text())
    except Exception:
        text = ""
    if status != 200:
        raise RecordError("push_refused", "The repository host refused the post (HTTP %d)." % status, 502)
    ok, reason = edits.report_status(text, "refs/heads/" + branch)
    if not ok:
        if any(word in reason for word in ("fetch first", "non-fast-forward", "stale", "lock")):
            raise Conflict()
        raise RecordError("push_refused", "The repository host refused the post: " + reason, 502)
    return new_commit


def _message(event, account, result):
    target = event["topic"] or result.get("topic") or ""
    words = {"topic": "New topic", "post": "Reply", "edit": "Edit", "delete": "Delete", "like": "Like",
             "unlike": "Unlike"}.get(event["type"], event["type"].replace("-", " ").capitalize())
    return "Forum: %s%s by @%s" % (words, (" in " + target) if target else "", account)


async def _viewer(env, request, owner, repo, data):
    _, record = await _account_session_record(env, request, data or {})
    account = clean_string((record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    moderator = bool(account) and await _moderation.can_moderate_repo(env, account, owner, repo)
    return account, moderator


async def _forum_branch(env, owner, repo):
    return await core_push_default_branch(env, owner, repo)


async def submit(env, request, owner, repo, data):
    features = await _repo_features_get(env, owner, repo)
    if features.get("forum") is not True:
        raise RecordError("forum_off", "The forum is turned off for this repository.", 403)
    branch = await _forum_branch(env, owner, repo)
    if not edits.valid_branch(branch):
        raise RecordError("invalid_branch", "The repository branch is not valid.")
    now = int(Date.now())
    event = clean_event(data.get("event"), now)
    record = dict(event, authorName=str((data.get("event") or {}).get("authorName") or ""))
    _, session_record = await _account_session_record(env, request, data, allow_unverified=True)
    session = clean_string((session_record or {}).get("name", ""), MAX_NODE_NAME).strip().lower()
    if not session:
        raise RecordError("sign_in", "Sign in to take part in the forum.", 401)
    refused = await _bind_session_author(env, request, data, record, session=session)
    if refused is not None:
        raise RecordError("author_mismatch", "This key belongs to another account.", 403)
    account = str(record.get("authorName") or "")
    if not record.get("authorVerified") or not account:
        raise RecordError("sign_in", "Sign in to take part in the forum.", 401)
    repo_key = "%s/%s" % (owner.lower(), repo.lower())
    if not await ed25519_verify(event["author"], event["sig"], canonical(repo_key, event).encode("utf-8")):
        raise RecordError("bad_signature", "The forum event's signature does not match.", 403)
    moderator = await _moderation.can_moderate_repo(env, account, owner, repo)
    if event["type"] in AUTHOR_TYPES and not moderator:
        posting = event["type"] in ("topic", "post")
        refusal, context = await _moderation.submission_gate(
            env, request, {}, owner, repo,
            {"type": "open" if event["type"] == "topic" else "comment", "authorName": account},
            surface="issue" if posting else "forum")
        if refusal is not None:
            raise RecordError("refused", "You cannot post in this forum right now.", 403)
        if context.get("hold"):
            raise RecordError("approval_required",
                              "Your first posts need an operator's approval, which this forum cannot take yet.", 403)
    if not moderator:
        throttled = await signup_rate_check(
            env, await blind_index(env, "forum-write\n" + account), MAX_WRITES_PER_ACCOUNT_PER_HOUR)
        if throttled is not None:
            raise RecordError("rate_limited", "That is a lot of forum activity; try again later.", 429)
    quota = await hosted_push_limit(env, request, owner, repo, "POST")
    if quota is not None:
        raise RecordError("repository_quota_exceeded", "This repository has reached its hosting limit.", 413)
    ctx = {"account": account, "moderator": moderator, "now": now, "repoTitle": repo}
    for _attempt in range(MAX_ATTEMPTS):
        commit, read = await read_forum(env, owner, repo, branch)
        changes, result = await apply_event(read, event, ctx)
        try:
            new_commit = await commit_records(
                env, owner, repo, branch, commit, changes, account, _message(event, account, result))
        except Conflict:
            continue
        if event["type"] in OPERATOR_TYPES:
            await _audit_sensitive_action(env, account, "forum." + event["type"], "repository", repo_key,
                                          "success", {"topic": event["topic"], "post": event["post"]})
        return {"ok": True, "commit": new_commit, "event": event["id"], **result}
    raise RecordError("busy", "The forum changed while saving; try again.", 409)


async def forum_handler(env, request, owner, repo):
    method = method_name(request)
    try:
        if method == "GET":
            account, moderator = await _viewer(env, request, owner, repo, {})
            return json_response({"ok": True, "account": account, "moderator": moderator},
                                 cache_control="no-store")
        if method != "POST":
            return json_response({"error": "method_not_allowed"}, status=405)
        try:
            data = await bounded_json_request(request)
        except Exception:
            data = None
        if not isinstance(data, dict):
            raise RecordError("invalid_json", "The request was not understood.")
        return json_response(await submit(env, request, owner, repo, data), cache_control="no-store")
    except RecordError as exc:
        return json_response({"error": exc.code, "message": exc.message}, status=exc.status,
                             cache_control="no-store")
