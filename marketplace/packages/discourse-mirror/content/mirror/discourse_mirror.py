#!/usr/bin/env python3
from __future__ import annotations

import argparse
import contextlib
import datetime
import fcntl
import hashlib
import html
import http.client
import json
import math
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import urllib.robotparser
from html.parser import HTMLParser
from pathlib import Path

PLUGIN_ID = "discourse-mirror"
FORMAT = 1
ROBOTS_AGENT = "ForkMeshDiscourseMirror"
USER_AGENT = f"{ROBOTS_AGENT}/1.0 (+https://forkmesh.com/market/discourse-mirror)"
NAME_RE = re.compile(r"[a-z0-9][a-z0-9-]{0,62}")
LOOPBACK = {"127.0.0.1", "localhost", "::1"}
POSTS_PER_REQUEST = 20
TOPICS_PER_PAGE = 100
LATEST_ON_INDEX = 50
MAX_JSON_BYTES = 16_000_000
MAX_RETRY_WAIT = 300
IMAGE_TYPES = {
    "image/png": ".png",
    "image/jpeg": ".jpg",
    "image/gif": ".gif",
    "image/webp": ".webp",
    "image/avif": ".avif",
}
DEFAULTS = {
    "delaySeconds": 1.0,
    "revisitPerRun": 50,
    "maxTopicsPerRun": 0,
    "uploads": True,
    "uploadHosts": [],
    "maxUploadBytes": 4_000_000,
    "maxUploadBytesPerRun": 100_000_000,
    "ignoreRobots": False,
    "pushRemote": "",
    "branch": "main",
    "commitName": "ForkMesh Discourse Mirror",
    "commitEmail": "discourse-mirror@forkmesh.invalid",
    "layout": "site",
}
FORUM_PATH = ".forkmesh/forum"
NATIVE_CATEGORY_BASE = 1_000_000
CI_SETTINGS = FORUM_PATH + "/source/settings.json"
CI_KEYS = ("uploads", "uploadHosts", "maxUploadBytes", "maxUploadBytesPerRun", "revisitPerRun", "delaySeconds",
           "commitName", "commitEmail")
FORUM_PAGE_SIZE = 50
FORUM_POSTS_PER_CHUNK = 100
FORUM_LATEST_PER_CATEGORY = 3
TOP_PERIODS = {"all": 0, "yearly": 365, "quarterly": 91, "monthly": 30, "weekly": 7, "daily": 1}
SAFE_KEY_RE = re.compile(r"[\w.-]{1,80}")
TOPIC_LINK_RE = re.compile(r"^/t/(?:[^/]+/)?(\d+)(?:/(\d+))?/?$")
DIMENSION_RE = re.compile(r"\d{1,5}")


class MirrorError(Exception):
    pass


class RedirectRefused(MirrorError):
    pass


class TimeUp(Exception):
    pass


def log(message: str) -> None:
    if os.environ.get("FORKMESH_DISCOURSE_MIRROR_QUIET") != "1":
        print(message, file=sys.stderr, flush=True)


def now_iso() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def normalize_origin(text: str) -> str:
    parsed = urllib.parse.urlsplit(str(text or "").strip())
    host = (parsed.hostname or "").lower()
    if (
        parsed.scheme not in ("https", "http")
        or not host
        or parsed.username
        or parsed.password
        or parsed.query
        or parsed.fragment
    ):
        raise MirrorError(f"not a forum address: {text!r}")
    if parsed.scheme == "http" and host not in LOOPBACK:
        raise MirrorError("only https forums can be mirrored")
    netloc = f"[{host}]" if ":" in host else host
    if parsed.port:
        netloc += f":{parsed.port}"
    return f"{parsed.scheme}://{netloc}{parsed.path.rstrip('/')}"


def default_data_dir() -> Path:
    override = os.environ.get("FORKMESH_DISCOURSE_MIRROR_DATA")
    if override:
        return Path(override).expanduser()
    tools = Path(__file__).resolve().parents[5] / "tools"
    if (tools / "forkmesh_paths.py").is_file():
        if str(tools) not in sys.path:
            sys.path.append(str(tools))
        import forkmesh_paths

        return forkmesh_paths.resolve().plugin_data(PLUGIN_ID)
    raise MirrorError("pass --data-dir: the ForkMesh project layout was not found")


def write_bytes(path: Path, data: bytes) -> bool:
    with contextlib.suppress(OSError):
        if path.read_bytes() == data:
            return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_bytes(data)
    os.replace(temporary, path)
    return True


def write_json(path: Path, value: object) -> bool:
    return write_bytes(path, (json.dumps(value, indent=1, ensure_ascii=False, sort_keys=True) + "\n").encode("utf-8"))


def read_json(path: Path, fallback: object) -> object:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return fallback
    except (OSError, ValueError) as error:
        raise MirrorError(f"could not read {path}: {error}") from error


def remove_file(path: Path) -> None:
    with contextlib.suppress(FileNotFoundError):
        path.unlink()


class Config:
    def __init__(self, data_dir: Path):
        self.data_dir = data_dir
        self.path = data_dir / "forums.json"
        raw = read_json(self.path, {"schemaVersion": 1, "forums": []})
        if not isinstance(raw, dict) or raw.get("schemaVersion") != 1 or not isinstance(raw.get("forums"), list):
            raise MirrorError(f"{self.path} is not a forum list")
        self.forums: list[dict] = [dict(item) for item in raw["forums"] if isinstance(item, dict)]

    def save(self) -> None:
        write_json(self.path, {"schemaVersion": 1, "forums": self.forums})

    def find(self, name: str) -> dict:
        for forum in self.forums:
            if forum.get("name") == name:
                return self.resolved(forum)
        raise MirrorError(f"no forum named {name!r} in {self.path}")

    def resolved(self, forum: dict) -> dict:
        merged = {**DEFAULTS, **forum}
        merged["origin"] = normalize_origin(merged.get("origin", ""))
        merged["repository"] = str(
            Path(merged.get("repository") or self.data_dir / "forums" / merged["name"]).expanduser()
        )
        return merged


class _Redirects(urllib.request.HTTPRedirectHandler):
    def __init__(self, client: "Client"):
        self.client = client

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        target = urllib.parse.urlsplit(newurl)
        if not self.client.allowed_url(newurl, target.hostname or ""):
            raise RedirectRefused(f"refused a redirect to {newurl}")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


class Client:
    def __init__(self, forum: dict, sleep=time.sleep):
        self.origin = forum["origin"]
        parts = urllib.parse.urlsplit(self.origin)
        self.scheme = parts.scheme
        self.host = (parts.hostname or "").lower()
        self.netloc = parts.netloc
        self.prefix = parts.path
        self.delay = max(0.0, float(forum.get("delaySeconds", DEFAULTS["delaySeconds"])))
        self.ignore_robots = bool(forum.get("ignoreRobots"))
        self.upload_hosts = {self.host} | {str(h).lower() for h in forum.get("uploadHosts") or []}
        self.sleep = sleep
        self.requests = 0
        self.last_request = 0.0
        self.robots: urllib.robotparser.RobotFileParser | None = None
        self.hosts = {self.host}
        self.opener = urllib.request.build_opener(_Redirects(self))

    def allowed_url(self, url: str, host: str) -> bool:
        scheme = urllib.parse.urlsplit(url).scheme
        if scheme != "https" and not (scheme == self.scheme == "http" and host.lower() in LOOPBACK):
            return False
        return host.lower() in self.hosts

    def url(self, path: str, query: list[tuple[str, str]] | None = None) -> str:
        text = f"{self.scheme}://{self.netloc}{self.prefix}{path}"
        return f"{text}?{urllib.parse.urlencode(query)}" if query else text

    def _pace(self) -> None:
        wait = self.last_request + self.delay - time.monotonic()
        if wait > 0:
            self.sleep(wait)
        self.last_request = time.monotonic()

    def _open(self, url: str, accept: str, max_bytes: int, hosts: set[str]):
        attempts = 0
        while True:
            attempts += 1
            self._pace()
            self.requests += 1
            self.hosts = hosts
            request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT, "Accept": accept})
            try:
                with self.opener.open(request, timeout=30) as response:
                    body = response.read(max_bytes + 1)
                    return response.status, response.headers, body
            except urllib.error.HTTPError as error:
                if error.code != 429 and error.code < 500:
                    with contextlib.suppress(OSError, http.client.HTTPException):
                        return error.code, error.headers, error.read(65_536)
                    return error.code, error.headers, b""
                wait = _retry_after(error.headers, attempts)
                if attempts >= 6 or wait > MAX_RETRY_WAIT:
                    raise MirrorError(f"{url} answered {error.code}; stopped for this run") from error
                log(f"{url} answered {error.code}; waiting {wait:.0f}s")
                self.sleep(wait)
            except (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException) as error:
                if attempts >= 4:
                    raise MirrorError(f"{url} could not be reached: {error}") from error
                self.sleep(min(MAX_RETRY_WAIT, 5 * 2 ** attempts))
            finally:
                self.hosts = {self.host}

    def load_robots(self) -> None:
        parser = urllib.robotparser.RobotFileParser()
        status, _, body = self._open(
            f"{self.scheme}://{self.netloc}/robots.txt", "text/plain", 512_000, {self.host}
        )
        if status in (401, 403):
            parser.disallow_all = True
        else:
            parser.parse(body.decode("utf-8", "replace").splitlines() if status == 200 else [])
        self.robots = parser

    def robots_allow(self, url: str) -> bool:
        if self.ignore_robots:
            return True
        if self.robots is None:
            self.load_robots()
        return self.robots.can_fetch(ROBOTS_AGENT, url)

    def json(self, path: str, query: list[tuple[str, str]] | None = None, missing_ok: bool = False):
        url = self.url(path, query)
        if not self.robots_allow(url):
            raise MirrorError(f"the forum's robots.txt does not allow {path}")
        status, _, body = self._open(url, "application/json", MAX_JSON_BYTES, {self.host})
        if status in (403, 404, 410) and missing_ok:
            if _discourse_error(body):
                return None
            raise MirrorError(f"{url} answered {status} without a forum error; stopped without removing anything")
        if status == 403:
            raise MirrorError(f"{url} needs a login; only public forums can be mirrored")
        if status != 200:
            raise MirrorError(f"{url} answered {status}")
        if len(body) > MAX_JSON_BYTES:
            raise MirrorError(f"{url} is larger than {MAX_JSON_BYTES} bytes")
        try:
            return json.loads(body)
        except ValueError as error:
            raise MirrorError(f"{url} did not answer JSON; is this a Discourse forum?") from error

    def image(self, url: str, max_bytes: int) -> tuple[bytes, str] | None:
        host = (urllib.parse.urlsplit(url).hostname or "").lower()
        self.hosts = self.upload_hosts
        allowed = self.allowed_url(url, host)
        self.hosts = {self.host}
        if not allowed or (host == self.host and not self.robots_allow(url)):
            return None
        try:
            status, headers, body = self._open(url, "image/*", max_bytes, self.upload_hosts)
        except RedirectRefused:
            return None
        kind = str(headers.get("Content-Type") or "").split(";")[0].strip().lower() if headers else ""
        if status != 200 or kind not in IMAGE_TYPES or len(body) > max_bytes:
            return None
        return body, IMAGE_TYPES[kind]


def _discourse_error(body: bytes) -> bool:
    try:
        value = json.loads(body)
    except ValueError:
        return False
    return isinstance(value, dict) and ("errors" in value or "error_type" in value)


def _retry_after(headers, attempts: int) -> float:
    with contextlib.suppress(TypeError, ValueError):
        return max(1.0, float((headers or {}).get("Retry-After")))
    return float(min(MAX_RETRY_WAIT, 5 * 2 ** attempts))


def _int(value, fallback: int = 0) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return fallback


def _tags(value) -> list[str]:
    names = []
    for tag in value or []:
        name = tag.get("name") if isinstance(tag, dict) else tag
        if isinstance(name, str) and name:
            names.append(name)
    return names


def category_record(raw: dict) -> dict:
    return {
        "id": _int(raw.get("id")),
        "name": str(raw.get("name") or ""),
        "slug": str(raw.get("slug") or ""),
        "color": str(raw.get("color") or ""),
        "text_color": str(raw.get("text_color") or ""),
        "description": str(raw.get("description_text") or ""),
        "parent_category_id": _int(raw.get("parent_category_id"), 0) or None,
        "position": _int(raw.get("position")),
    }


def post_record(raw: dict) -> dict:
    return {
        "id": _int(raw.get("id")),
        "post_number": _int(raw.get("post_number")),
        "post_type": _int(raw.get("post_type"), 1),
        "username": str(raw.get("username") or ""),
        "name": str(raw.get("name") or ""),
        "created_at": str(raw.get("created_at") or ""),
        "updated_at": str(raw.get("updated_at") or ""),
        "reply_to_post_number": _int(raw.get("reply_to_post_number"), 0) or None,
        "action_code": str(raw.get("action_code") or ""),
        "cooked": str(raw.get("cooked") or ""),
        "like_count": _like_count(raw),
        "reads": _int(raw.get("reads")),
        "reply_count": _int(raw.get("reply_count")),
        "hidden": bool(raw.get("hidden")),
    }


def _like_count(raw: dict) -> int:
    for action in raw.get("actions_summary") or []:
        if isinstance(action, dict) and _int(action.get("id")) == 2:
            return _int(action.get("count"))
    return _int(raw.get("like_count"))


def _posters(raw: dict, users: dict[int, dict] | None) -> list[dict]:
    posters = []
    for poster in raw.get("posters") or []:
        if not isinstance(poster, dict):
            continue
        user = (users or {}).get(_int(poster.get("user_id"))) or {}
        username = str(user.get("username") or "")
        if username:
            posters.append({
                "username": username,
                "name": str(user.get("name") or ""),
                "description": str(poster.get("description") or ""),
            })
    return posters[:5]


LISTING_ONLY = ("posters", "excerpt", "last_poster_username")


def listing_fields(raw: dict, users: dict[int, dict] | None = None) -> dict:
    return {
        "id": _int(raw.get("id")),
        "title": str(raw.get("title") or ""),
        "slug": str(raw.get("slug") or ""),
        "category_id": _int(raw.get("category_id"), 0) or None,
        "tags": _tags(raw.get("tags")),
        "created_at": str(raw.get("created_at") or ""),
        "bumped_at": str(raw.get("bumped_at") or ""),
        "last_posted_at": str(raw.get("last_posted_at") or ""),
        "posts_count": _int(raw.get("posts_count")),
        "highest_post_number": _int(raw.get("highest_post_number")),
        "pinned": bool(raw.get("pinned")),
        "pinned_globally": bool(raw.get("pinned_globally")),
        "closed": bool(raw.get("closed")),
        "archived": bool(raw.get("archived")),
        "visible": raw.get("visible") is not False,
        "views": _int(raw.get("views")),
        "like_count": _int(raw.get("like_count")),
        "reply_count": _int(raw.get("reply_count")),
        "excerpt": html.unescape(re.sub(r"<[^>]*>", "", str(raw.get("excerpt") or ""))).strip()[:400],
        "last_poster_username": str(raw.get("last_poster_username") or ""),
        "posters": _posters(raw, users),
    }


def _same_listing(old: dict | None, row: dict) -> bool:
    return bool(old) and old.get("fetched_at") and all(
        old.get(key) == row[key] for key in ("bumped_at", "highest_post_number", "posts_count")
    )


class Store:
    def __init__(self, root: Path):
        self.root = root
        self.topics_dir = root / "topics"
        self.uploads_dir = root / "uploads"
        self.site = root / "site"
        rows = read_json(self.topics_dir / "index.json", [])
        self.index: dict[int, dict] = {int(row["id"]): row for row in rows if isinstance(row, dict) and "id" in row}
        uploads = read_json(self.uploads_dir / "index.json", {})
        self.uploads: dict[str, str | None] = uploads if isinstance(uploads, dict) else {}
        self.meta = read_json(root / "mirror.json", {})
        self.categories = read_json(root / "categories.json", [])

    def topic(self, topic_id: int) -> dict | None:
        value = read_json(self.topics_dir / f"{topic_id}.json", None)
        return value if isinstance(value, dict) else None

    def put_topic(self, topic: dict) -> None:
        write_json(self.topics_dir / f"{topic['id']}.json", topic)

    def drop_topic(self, topic_id: int) -> None:
        self.index.pop(topic_id, None)
        remove_file(self.topics_dir / f"{topic_id}.json")
        remove_file(self.site / "t" / f"{topic_id}.html")

    def save(self) -> None:
        write_json(self.topics_dir / "index.json", [self.index[key] for key in sorted(self.index)])
        write_json(self.uploads_dir / "index.json", self.uploads)
        write_json(self.root / "categories.json", self.categories)
        write_json(self.root / "mirror.json", self.meta)

    def collect_uploads(self) -> int:
        referenced = {name for row in self.index.values() for name in row.get("uploads") or []}
        removed = 0
        for url, name in list(self.uploads.items()):
            if name and name not in referenced:
                remove_file(self.uploads_dir / name)
                del self.uploads[url]
                removed += 1
        return removed


LIST_FIELDS = (
    "id", "title", "slug", "category_id", "tags", "created_at", "bumped_at", "last_posted_at", "posts_count",
    "reply_count", "views", "like_count", "pinned", "pinned_globally", "closed", "archived", "visible", "excerpt",
    "posters", "last_poster_username",
)


def _epoch(value: str) -> float:
    with contextlib.suppress(ValueError, TypeError):
        return datetime.datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp()
    return 0.0


def list_row(row: dict) -> dict:
    return {**{key: row.get(key) for key in LIST_FIELDS}, "origin": row.get("origin") or "discourse"}


STATE_KEYS = ("pinned", "pinned_globally", "closed", "archived", "visible", "category_id", "title")


def merge_local_state(row: dict, state: dict) -> dict:
    merged = dict(row)
    for key in STATE_KEYS:
        if key in state:
            merged[key] = state[key]
    local = _int(state.get("local_posts"))
    if local:
        merged["posts_count"] = _int(merged.get("posts_count")) + local
        merged["reply_count"] = _int(merged.get("reply_count")) + local
    if str(state.get("bumped_at") or "") > str(merged.get("bumped_at") or ""):
        merged["bumped_at"] = state["bumped_at"]
        merged["last_posted_at"] = state["bumped_at"]
        merged["last_poster_username"] = state.get("last_poster_username") or merged.get("last_poster_username", "")
    posters = list(merged.get("posters") or [])
    for poster in state.get("posters") or []:
        if isinstance(poster, dict) and all(item.get("username") != poster.get("username") for item in posters):
            posters.append(poster)
    merged["posters"] = posters[:5]
    return merged


def latest_row(row: dict) -> dict:
    return {key: row.get(key) for key in ("id", "title", "slug", "bumped_at", "last_posted_at", "posts_count",
                                          "pinned", "closed", "archived")}


def tag_key(tag: str) -> str:
    if SAFE_KEY_RE.fullmatch(tag) and not tag.startswith("."):
        return tag
    return "h-" + hashlib.sha256(tag.encode("utf-8")).hexdigest()[:16]


def _bumped(row: dict) -> tuple:
    return (row.get("bumped_at") or "", row["id"])


class ForumStore(Store):
    def __init__(self, root: Path, origin: str):
        self.root = root
        self.origin = origin
        self.topics_dir = root / "topics"
        self.uploads_dir = root / "uploads"
        self.site = root / "site"
        cursor = read_json(root / "source" / "discourse.json", {})
        cursor = cursor if isinstance(cursor, dict) else {}
        self.index = {
            int(row["id"]): row for row in cursor.get("topics") or [] if isinstance(row, dict) and "id" in row
        }
        uploads = cursor.get("uploads")
        self.uploads = uploads if isinstance(uploads, dict) else {}
        forum = read_json(root / "forum.json", {})
        forum = forum if isinstance(forum, dict) else {}
        source = forum.get("source") if isinstance(forum.get("source"), dict) else {}
        self.meta = {
            "title": forum.get("title") or "",
            "description": forum.get("description") or "",
            "source": source.get("origin") or origin,
            "pending": source.get("pending", 1),
        }
        categories = read_json(root / "categories.json", [])
        self.categories = categories if isinstance(categories, list) else []

    def topic_dir(self, topic_id: int) -> Path:
        return self.topics_dir / str(topic_id)

    def topic(self, topic_id: int) -> dict | None:
        directory = self.topic_dir(topic_id)
        value = read_json(directory / "topic.json", None)
        if not isinstance(value, dict):
            return None
        posts = list(value.get("posts") or [])
        for number in range(1, _int(value.get("chunks"), 1)):
            chunk = read_json(directory / "posts" / f"{number}.json", {})
            posts += (chunk.get("posts") if isinstance(chunk, dict) else None) or []
        return {**value, "posts": posts}

    def put_topic(self, topic: dict) -> None:
        directory = self.topic_dir(topic["id"])
        posts = [{**post, "format": "html", "origin": "discourse"} for post in topic["posts"]]
        chunks = max(1, math.ceil(len(posts) / FORUM_POSTS_PER_CHUNK))
        uploads = {}
        for post in posts:
            for url in image_sources(post["cooked"], self.origin):
                if self.uploads.get(url):
                    uploads[url] = self.uploads[url]
        header = {key: value for key, value in topic.items() if key != "posts"}
        header.update(origin="discourse", chunks=chunks, uploads=uploads, posts=posts[:FORUM_POSTS_PER_CHUNK])
        write_json(directory / "topic.json", header)
        for number in range(1, chunks):
            start = number * FORUM_POSTS_PER_CHUNK
            write_json(directory / "posts" / f"{number}.json", {"posts": posts[start:start + FORUM_POSTS_PER_CHUNK]})
        for stale in (directory / "posts").glob("*.json"):
            if not stale.stem.isdigit() or int(stale.stem) >= chunks:
                remove_file(stale)

    def drop_topic(self, topic_id: int) -> None:
        self.index.pop(topic_id, None)
        directory = self.topic_dir(topic_id)
        for stale in (directory / "posts").glob("*.json"):
            remove_file(stale)
        remove_file(directory / "topic.json")
        for folder in (directory / "posts", directory):
            with contextlib.suppress(OSError):
                folder.rmdir()

    def save(self) -> None:
        write_json(self.root / "source" / "discourse.json", {
            "format": FORMAT,
            "topics": [self.index[key] for key in sorted(self.index)],
            "uploads": self.uploads,
        })
        ForumViews(self).write()


class ForumViews:
    def __init__(self, store: ForumStore):
        self.store = store
        self.root = store.root
        self.local = read_json(self.root / "local.json", {})
        self.local = self.local if isinstance(self.local, dict) else {}
        imported = []
        for row in store.index.values():
            if not row.get("fetched_at"):
                continue
            state = read_json(self.root / "topics" / str(row["id"]) / "state.json", {})
            imported.append(merge_local_state(row, state) if isinstance(state, dict) and state else row)
        native = read_json(self.root / "native.json", {})
        native_rows = [dict(row, origin="forkmesh") for row in ((native or {}).get("topics") or {}).values()
                       if isinstance(row, dict) and row.get("id") and not row.get("deleted")]
        self.rows = imported + native_rows
        self.native_posts = sum(_int(row.get("posts_count")) for row in native_rows)
        self.listed = [row for row in self.rows if row.get("visible", True)]

    def write(self) -> None:
        categories = self.categories()
        write_json(self.root / "categories.json", categories)
        meta = self.store.meta
        origin = str(meta.get("source") or self.store.origin or "")
        write_json(self.root / "forum.json", {
            "format": FORMAT,
            "kind": "forkmesh-forum",
            "generator": f"ForkMesh {PLUGIN_ID}",
            "title": str(self.local.get("title") or meta.get("title") or ""),
            "description": str(self.local.get("description") if "description" in self.local else meta.get("description") or ""),
            "source": {
                "type": "discourse" if origin else "forkmesh",
                "origin": origin,
                "lastSync": str(meta.get("lastSync") or ""),
                "pending": _int(meta.get("pending")),
            },
            "counts": {
                "topics": len(self.rows),
                "posts": sum(_int(row.get("mirrored_posts")) for row in self.rows) + self.native_posts,
                "categories": len(categories),
            },
            "pageSize": FORUM_PAGE_SIZE,
            "postsPerChunk": FORUM_POSTS_PER_CHUNK,
            "topPeriods": list(TOP_PERIODS),
        })
        ordered = sorted(self.listed, key=_bumped, reverse=True)
        global_pins = [row for row in ordered if row.get("pinned_globally")]
        self.write_pages(self.root / "latest", global_pins + [row for row in ordered if not row.get("pinned_globally")])
        kept = set()
        for category in categories:
            family = {category["id"], *category["subcategory_ids"]}
            rows = [row for row in ordered if row.get("category_id") in family]
            pins = [row for row in rows if row.get("pinned")]
            self.write_pages(self.root / "c" / str(category["id"]), pins + [row for row in rows if not row.get("pinned")])
            kept.add(str(category["id"]))
        self.prune_dirs(self.root / "c", kept)
        tags: dict[str, int] = {}
        for row in self.listed:
            for tag in row.get("tags") or []:
                tags[tag] = tags.get(tag, 0) + 1
        tag_list = sorted(({"name": name, "key": tag_key(name), "count": count} for name, count in tags.items()),
                          key=lambda item: (-item["count"], item["name"]))
        write_json(self.root / "tags.json", tag_list)
        for tag in tag_list:
            self.write_pages(self.root / "tag" / tag["key"], [row for row in ordered if tag["name"] in (row.get("tags") or [])])
        self.prune_dirs(self.root / "tag", {tag["key"] for tag in tag_list})
        now = time.time()
        for period, days in TOP_PERIODS.items():
            rows = [row for row in self.listed if not days or now - _epoch(row.get("created_at")) <= days * 86400]
            rows.sort(key=lambda row: (_int(row.get("like_count")) * 15 + _int(row.get("posts_count")) * 5
                                       + _int(row.get("views")) * 0.05, row["id"]), reverse=True)
            write_json(self.root / "top" / f"{period}.json",
                       {"period": period, "rows": [list_row(row) for row in rows[:FORUM_PAGE_SIZE]]})

    def categories(self) -> list[dict]:
        items = [dict(item) for item in self.store.categories if isinstance(item, dict) and item.get("id")]
        by_id = {item["id"]: item for item in items}
        for key, override in (self.local.get("categories") or {}).items():
            if not isinstance(override, dict) or not str(key).isdigit():
                continue
            category_id = int(key)
            if category_id not in by_id:
                if category_id < NATIVE_CATEGORY_BASE:
                    continue
                by_id[category_id] = {"id": category_id, "text_color": "FFFFFF", "origin": "forkmesh"}
                items.append(by_id[category_id])
            by_id[category_id].update({field: override[field] for field in (
                "name", "slug", "color", "description", "parent_category_id", "position") if field in override})
        known = {item["id"] for item in items}
        for item in items:
            if item.get("parent_category_id") not in known:
                item["parent_category_id"] = None
        children: dict[int, list[int]] = {}
        for item in sorted(items, key=lambda value: (value.get("position", 0), value["id"])):
            if item["parent_category_id"]:
                children.setdefault(item["parent_category_id"], []).append(item["id"])
        ordered = sorted(self.listed, key=_bumped, reverse=True)
        for item in items:
            own = [row for row in self.listed if row.get("category_id") == item["id"]]
            family = {item["id"], *children.get(item["id"], [])}
            members = [row for row in ordered if row.get("category_id") in family]
            item.update(
                subcategory_ids=children.get(item["id"], []),
                topic_count=len(own),
                total_topic_count=len(members),
                post_count=sum(_int(row.get("posts_count")) for row in own),
                latest=[latest_row(row) for row in members[:FORUM_LATEST_PER_CATEGORY]],
            )
        return sorted(items, key=lambda value: (value.get("position", 0), value["id"]))

    def write_pages(self, directory: Path, rows: list[dict]) -> None:
        pages = max(1, math.ceil(len(rows) / FORUM_PAGE_SIZE))
        for number in range(1, pages + 1):
            chunk = rows[(number - 1) * FORUM_PAGE_SIZE:number * FORUM_PAGE_SIZE]
            write_json(directory / f"{number}.json", {
                "page": number, "pages": pages, "more": number < pages, "rows": [list_row(row) for row in chunk],
            })
        for stale in directory.glob("*.json"):
            if not stale.stem.isdigit() or int(stale.stem) > pages:
                remove_file(stale)

    def prune_dirs(self, parent: Path, keep: set[str]) -> None:
        if not parent.is_dir():
            return
        for child in parent.iterdir():
            if child.is_dir() and child.name not in keep:
                for stale in child.glob("*.json"):
                    remove_file(stale)
                with contextlib.suppress(OSError):
                    child.rmdir()


class _ImageSources(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.sources: list[str] = []

    def handle_starttag(self, tag, attrs):
        if tag == "img":
            source = dict(attrs).get("src")
            if source:
                self.sources.append(source)


def absolute_url(origin: str, value: str) -> str:
    value = str(value or "").strip()
    if not value or value.lower().startswith(("javascript:", "data:", "vbscript:")):
        return ""
    return urllib.parse.urljoin(origin + "/", value)


def image_sources(cooked: str, origin: str) -> list[str]:
    parser = _ImageSources()
    parser.feed(cooked)
    parser.close()
    found = []
    for source in parser.sources:
        absolute = absolute_url(origin, source)
        if absolute.startswith(("https://", "http://")) and absolute not in found:
            found.append(absolute)
    return found


class Mirror:
    def __init__(self, forum: dict, sleep=time.sleep):
        self.forum = forum
        self.origin = forum["origin"]
        self.client = Client(forum, sleep=sleep)
        self.forum_layout = forum.get("layout") == "forum"
        if self.forum_layout:
            self.store = ForumStore(Path(forum["repository"]) / FORUM_PATH, self.origin)
        else:
            self.store = Store(Path(forum["repository"]))
        self.upload_budget = int(forum["maxUploadBytesPerRun"])
        self.stats = {"fetched": 0, "revisited": 0, "removed": 0, "uploads": 0, "pending": 0}
        self.rendered: list[int] = []
        self.deadline: float | None = None

    def sync(self, full: bool = False, max_topics: int | None = None) -> dict:
        ensure_repository(Path(self.forum["repository"]), self.forum["branch"], create=not self.forum_layout)
        site = self.client.json("/site.json")
        about = (self.client.json("/about.json", missing_ok=True) or {}).get("about") or {}
        previous = dict(self.store.meta) if isinstance(self.store.meta, dict) else {}
        self.store.meta = {
            "format": FORMAT,
            "generator": f"ForkMesh {PLUGIN_ID}",
            "source": self.origin,
            "title": str(about.get("title") or previous.get("title") or self.origin),
            "description": str(about.get("description") or previous.get("description") or ""),
        }
        self.store.categories = sorted(
            (category_record(item) for item in site.get("categories") or [] if isinstance(item, dict)),
            key=lambda item: (item["position"], item["id"]),
        )
        changed, unseen = self.walk_listing(full)
        limit = self.forum["maxTopicsPerRun"] if max_topics is None else max_topics
        changed_set = set(changed)
        queue = changed + [key for key, row in self.store.index.items() if not row.get("fetched_at") and key not in changed_set]
        if limit:
            queue = queue[:limit]
        revisit = self.revisit_candidates(set(queue), unseen)
        error = None
        try:
            for topic_id in queue:
                self.check_time()
                self.fetch_topic(topic_id, refresh=False)
            for topic_id in revisit:
                self.check_time()
                self.fetch_topic(topic_id, refresh=True)
                self.stats["revisited"] += 1
        except MirrorError as stopped:
            error = str(stopped)
            log(error)
        except TimeUp:
            self.stats["stoppedAtTimeLimit"] = True
        self.stats["pending"] = sum(1 for row in self.store.index.values() if not row.get("fetched_at"))
        self.store.collect_uploads()
        self.store.meta.update(
            lastSync=now_iso(),
            topics=sum(1 for row in self.store.index.values() if row.get("fetched_at")),
            posts=sum(row.get("mirrored_posts", 0) for row in self.store.index.values()),
            pending=self.stats["pending"],
        )
        self.store.save()
        if self.forum_layout:
            commit = commit_repository(self.forum, self.summary_line())
            return {**self.stats, "requests": self.client.requests, "commit": commit, "error": error}
        renderer = Renderer(self.store)
        finished_first_copy = self.stats["pending"] == 0 and previous.get("pending", 1) != 0
        if finished_first_copy:
            renderer.render()
        else:
            for topic_id in self.rendered:
                topic = self.store.topic(topic_id)
                if topic:
                    renderer.render_topic(topic)
            renderer.render(changed_only=True)
        commit = commit_repository(self.forum, self.summary_line())
        return {**self.stats, "requests": self.client.requests, "commit": commit, "error": error}

    def check_time(self) -> None:
        if self.deadline is not None and time.monotonic() >= self.deadline:
            raise TimeUp()

    def summary_line(self) -> str:
        parts = [f"{self.stats['fetched']} topics updated"]
        if self.stats["removed"]:
            parts.append(f"{self.stats['removed']} removed")
        if self.stats["pending"]:
            parts.append(f"{self.stats['pending']} still to copy")
        return f"Mirror {self.store.meta.get('title')}: " + ", ".join(parts)

    def walk_listing(self, full: bool) -> tuple[list[int], set[int]]:
        changed: list[int] = []
        seen: set[int] = set()
        page = 0
        incremental = bool(self.store.index) and not full
        while True:
            data = self.client.json("/latest.json", [("page", str(page))]) or {}
            topic_list = data.get("topic_list") or {}
            users = {_int(user.get("id")): user for user in data.get("users") or [] if isinstance(user, dict)}
            rows = [listing_fields(item, users) for item in topic_list.get("topics") or [] if isinstance(item, dict)]
            fresh = False
            new_ids = False
            for row in rows:
                if not row["id"] or row["id"] in seen:
                    continue
                seen.add(row["id"])
                new_ids = True
                old = self.store.index.get(row["id"])
                if not _same_listing(old, row):
                    changed.append(row["id"])
                    fresh = fresh or not row["pinned"]
                self.store.index[row["id"]] = {**(old or {}), **row, "fetched_at": (old or {}).get("fetched_at")}
            if not new_ids or not topic_list.get("more_topics_url") or (incremental and not fresh):
                break
            page += 1
        unseen = set(self.store.index) - seen if not incremental else set()
        return changed, unseen

    def revisit_candidates(self, queued: set[int], unseen: set[int]) -> list[int]:
        count = max(0, int(self.forum["revisitPerRun"]))
        rows = [row for key, row in self.store.index.items() if key not in queued and row.get("fetched_at")]
        chosen = sorted(row["id"] for row in rows if row["id"] in unseen)
        others = sorted((row for row in rows if row["id"] not in unseen), key=lambda row: (row["fetched_at"], row["id"]))
        return chosen + [row["id"] for row in others[:count]]

    def fetch_topic(self, topic_id: int, refresh: bool) -> None:
        data = self.client.json(f"/t/{topic_id}.json", missing_ok=True)
        if not data or _int(data.get("id")) != topic_id:
            self.remove_topic(topic_id)
            return
        stream = [_int(item) for item in (data.get("post_stream") or {}).get("stream") or []]
        posts = {post["id"]: post for post in map(post_record, (data.get("post_stream") or {}).get("posts") or [])}
        if not refresh:
            for post in (self.store.topic(topic_id) or {}).get("posts") or []:
                posts.setdefault(post["id"], post)
        missing = [post_id for post_id in stream if post_id not in posts]
        for start in range(0, len(missing), POSTS_PER_REQUEST):
            chunk = missing[start:start + POSTS_PER_REQUEST]
            more = self.client.json(
                f"/t/{topic_id}/posts.json", [("post_ids[]", str(post_id)) for post_id in chunk]
            ) or {}
            for post in map(post_record, (more.get("post_stream") or {}).get("posts") or []):
                posts[post["id"]] = post
        ordered = [posts[post_id] for post_id in stream if post_id in posts]
        details = data.get("details") or {}
        topic = {
            **listing_fields(data),
            "participant_count": _int(data.get("participant_count")) or len(details.get("participants") or []),
            "link_count": len(details.get("links") or []),
            "source": f"{self.origin}/t/{data.get('slug') or 'topic'}/{topic_id}",
            "posts": ordered,
        }
        uploads = self.fetch_uploads(ordered)
        self.store.put_topic(topic)
        listed = self.store.index.get(topic_id) or {}
        row = {key: topic[key] for key in listing_fields({})}
        for key in ("bumped_at", "highest_post_number", "posts_count", *LISTING_ONLY):
            if listed.get(key):
                row[key] = listed[key]
        if not row.get("posters") and ordered:
            row["posters"] = [{"username": ordered[0]["username"], "name": ordered[0]["name"],
                               "description": "Original Poster"}]
        if not row.get("last_poster_username") and ordered:
            row["last_poster_username"] = ordered[-1]["username"]
        row.update(
            fetched_at=now_iso(),
            mirrored_posts=len(ordered),
            author=ordered[0]["username"] if ordered else "",
            uploads=uploads,
        )
        self.store.index[topic_id] = row
        self.rendered.append(topic_id)
        self.stats["fetched"] += 1
        if self.stats["fetched"] % 500 == 0:
            self.store.save()

    def remove_topic(self, topic_id: int) -> None:
        if topic_id not in self.store.index:
            return
        if self.stats["removed"] >= 25 and self.stats["removed"] > self.stats["fetched"]:
            raise MirrorError("too many topics vanished at once; stopped without removing more")
        self.store.drop_topic(topic_id)
        self.stats["removed"] += 1

    def fetch_uploads(self, posts: list[dict]) -> list[str]:
        if not self.forum["uploads"]:
            return []
        names = []
        for post in posts:
            for url in image_sources(post["cooked"], self.origin):
                host = (urllib.parse.urlsplit(url).hostname or "").lower()
                if host not in self.client.upload_hosts:
                    continue
                if url not in self.store.uploads and self.upload_budget > 0:
                    self.store.uploads[url] = self.download(url)
                name = self.store.uploads.get(url)
                if name and name not in names:
                    names.append(name)
        return names

    def download(self, url: str) -> str | None:
        result = self.client.image(url, min(int(self.forum["maxUploadBytes"]), self.upload_budget))
        if not result:
            return None
        body, extension = result
        name = hashlib.sha256(url.encode("utf-8")).hexdigest()[:32] + extension
        write_bytes(self.store.uploads_dir / name, body)
        self.upload_budget -= len(body)
        self.stats["uploads"] += 1
        return name


ALLOWED_TAGS = {
    "a": {"href", "title"},
    "img": {"src", "alt", "title", "width", "height"},
    "ol": {"start"},
    "th": {"colspan", "rowspan"},
    "td": {"colspan", "rowspan"},
    "abbr": {"title"},
    "details": {"open"},
    **{
        tag: set()
        for tag in (
            "p", "br", "hr", "b", "strong", "i", "em", "u", "s", "strike", "del", "ins", "code",
            "pre", "blockquote", "ul", "li", "h1", "h2", "h3", "h4", "h5", "h6", "table", "thead",
            "tbody", "tfoot", "tr", "sup", "sub", "summary", "span", "div", "aside", "kbd", "mark",
            "small", "cite", "dl", "dt", "dd",
        )
    },
}
DROPPED_WITH_CONTENT = {
    "script", "style", "iframe", "object", "embed", "svg", "math", "template", "noscript", "form",
    "textarea", "select", "button", "video", "audio", "canvas", "frame", "frameset", "head", "title",
}
VOID_TAGS = {"br", "hr", "img"}


class Sanitizer(HTMLParser):
    def __init__(self, origin: str, uploads: dict, topics: set[int], upload_prefix: str, topic_prefix: str):
        super().__init__(convert_charrefs=True)
        self.origin = origin
        parts = urllib.parse.urlsplit(origin)
        self.host = (parts.hostname or "").lower()
        self.path_prefix = parts.path
        self.uploads = uploads
        self.topics = topics
        self.upload_prefix = upload_prefix
        self.topic_prefix = topic_prefix
        self.out: list[str] = []
        self.open: list[str] = []
        self.skipping: list[str] = []

    def handle_starttag(self, tag, attrs):
        self._start(tag, attrs, closed=False)

    def handle_startendtag(self, tag, attrs):
        self._start(tag, attrs, closed=True)

    def _start(self, tag, attrs, closed):
        if self.skipping:
            if tag in DROPPED_WITH_CONTENT and not closed:
                self.skipping.append(tag)
            return
        if tag in DROPPED_WITH_CONTENT:
            if not closed:
                self.skipping.append(tag)
            return
        allowed = ALLOWED_TAGS.get(tag)
        if allowed is None:
            return
        values = {name: value for name, value in attrs if name in allowed and value is not None}
        if tag == "img":
            self._image(values)
            return
        if tag == "a":
            href = self._link(values.get("href", ""))
            values = {"title": values["title"]} if "title" in values else {}
            if href:
                values["href"] = href
                if href.startswith(("https://", "http://")):
                    values["rel"] = "nofollow noopener noreferrer"
        for name in ("colspan", "rowspan", "start"):
            if name in values and not DIMENSION_RE.fullmatch(values[name]):
                del values[name]
        self._emit(tag, values)
        if closed and tag not in VOID_TAGS:
            self.out.append(f"</{tag}>")
        elif tag not in VOID_TAGS:
            self.open.append(tag)

    def _emit(self, tag, values):
        rendered = "".join(f' {name}="{html.escape(value, quote=True)}"' for name, value in sorted(values.items()))
        self.out.append(f"<{tag}{rendered}>")

    def _absolute(self, value: str) -> str:
        return absolute_url(self.origin, value)

    def _link(self, value: str) -> str:
        if value.startswith("#"):
            return value if re.fullmatch(r"#[A-Za-z0-9_-]{1,64}", value) else ""
        if value.lower().startswith("mailto:"):
            return value
        absolute = self._absolute(value)
        parts = urllib.parse.urlsplit(absolute)
        if parts.scheme not in ("https", "http"):
            return ""
        if (parts.hostname or "").lower() == self.host and parts.path.startswith(self.path_prefix):
            match = TOPIC_LINK_RE.match(parts.path[len(self.path_prefix):])
            if match and int(match.group(1)) in self.topics:
                anchor = f"#p{match.group(2)}" if match.group(2) else ""
                return f"{self.topic_prefix}{match.group(1)}.html{anchor}"
        return absolute

    def _image(self, values):
        source = self._absolute(values.get("src", ""))
        local = self.uploads.get(source)
        alt = values.get("alt", "")
        if local:
            kept = {"src": f"{self.upload_prefix}{local}", "alt": alt, "loading": "lazy"}
            for name in ("title", "width", "height"):
                if name in values and (name == "title" or DIMENSION_RE.fullmatch(values[name])):
                    kept[name] = values[name]
            self._emit("img", kept)
        elif source.startswith(("https://", "http://")):
            label = alt or "image"
            self.out.append(
                f'<a class="remote-image" href="{html.escape(source, quote=True)}" '
                f'rel="nofollow noopener noreferrer">[{html.escape(label)}]</a>'
            )
        elif alt:
            self.out.append(html.escape(alt))

    def handle_endtag(self, tag):
        if self.skipping:
            if tag == self.skipping[-1]:
                self.skipping.pop()
            return
        if tag in self.open:
            while self.open:
                name = self.open.pop()
                self.out.append(f"</{name}>")
                if name == tag:
                    break

    def handle_data(self, data):
        if not self.skipping:
            self.out.append(html.escape(data, quote=False))

    def result(self) -> str:
        self.close()
        while self.open:
            self.out.append(f"</{self.open.pop()}>")
        return "".join(self.out)


def sanitize(cooked: str, origin: str, uploads: dict, topics: set[int],
             upload_prefix: str = "../../uploads/", topic_prefix: str = "") -> str:
    parser = Sanitizer(origin, uploads, topics, upload_prefix, topic_prefix)
    parser.feed(cooked)
    return parser.result()


STYLE = """:root{color-scheme:light dark;--fg:#1f2328;--muted:#59636e;--line:#d1d9e0;--bg:#fff;--soft:#f6f8fa;--link:#0969da}
@media (prefers-color-scheme:dark){:root{--fg:#e6edf3;--muted:#9198a1;--line:#3d444d;--bg:#0d1117;--soft:#151b23;--link:#4493f8}}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.55 system-ui,-apple-system,"Segoe UI",sans-serif}
main{max-width:860px;margin:0 auto;padding:24px 16px 64px}
a{color:var(--link)}
header.page{border-bottom:1px solid var(--line);margin-bottom:16px;padding-bottom:12px}
header.page h1{font-size:1.6em;margin:.2em 0}
.muted,.meta{color:var(--muted);font-size:.9em}
.notice{background:var(--soft);border:1px solid var(--line);border-radius:6px;padding:8px 12px;font-size:.9em}
table.topics{width:100%;border-collapse:collapse}
table.topics td,table.topics th{border-bottom:1px solid var(--line);padding:8px 4px;text-align:left;vertical-align:top}
table.topics td.num{text-align:right;white-space:nowrap;color:var(--muted)}
article.post{border-bottom:1px solid var(--line);padding:16px 0}
article.post.small{color:var(--muted);font-size:.9em;padding:8px 0}
article.post .body img{max-width:100%;height:auto}
article.post pre{background:var(--soft);padding:8px;overflow:auto;border-radius:6px}
article.post blockquote,article.post aside{border-left:3px solid var(--line);margin:8px 0;padding:4px 12px;color:var(--muted)}
.tag{display:inline-block;border:1px solid var(--line);border-radius:4px;padding:0 6px;margin-right:4px;font-size:.85em}
nav.pages a,nav.pages span{margin-right:8px}
"""
CSP = "default-src 'none'; img-src 'self'; style-src 'self'; base-uri 'none'; form-action 'none'"


def _when(value: str) -> str:
    return html.escape(value[:16].replace("T", " ") + " UTC") if value else ""


def _page(title: str, css: str, body: str, home: str) -> str:
    return (
        "<!doctype html>\n<html lang=\"und\"><head><meta charset=\"utf-8\">"
        f"<meta http-equiv=\"Content-Security-Policy\" content=\"{CSP}\">"
        "<meta name=\"referrer\" content=\"no-referrer\"><meta name=\"robots\" content=\"noindex\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        f"<title>{html.escape(title)}</title><link rel=\"stylesheet\" href=\"{css}\"></head>"
        f"<body><main>{body}<p class=\"muted\"><a href=\"{home}\">Mirror home</a></p></main></body></html>\n"
    )


class Renderer:
    def __init__(self, store: Store):
        self.store = store
        self.origin = str(store.meta.get("source") or "")
        self.title = str(store.meta.get("title") or self.origin)
        self.categories = {item["id"]: item for item in store.categories if isinstance(item, dict)}
        self.mirrored = {key for key, row in store.index.items() if row.get("fetched_at")}

    def notice(self) -> str:
        source = html.escape(self.origin, quote=True)
        synced = _when(str(self.store.meta.get("lastSync") or ""))
        return (
            f'<p class="notice">A read-only copy of <a href="{source}" rel="nofollow noopener noreferrer">'
            f"{source}</a>, last copied {synced}. Posts belong to their authors under the source "
            "forum's terms. Reply, like or report on the forum itself.</p>"
        )

    def category_label(self, category_id, prefix: str) -> str:
        category = self.categories.get(category_id)
        if not category:
            return ""
        return f'<a href="{prefix}{category["id"]}.html">{html.escape(category["name"])}</a>'

    def topic_rows(self, rows: list[dict], topic_prefix: str, category_prefix: str) -> str:
        lines = [
            "<table class=\"topics\"><thead><tr><th>Topic</th><th>Category</th>"
            "<th class=\"num\">Posts</th><th class=\"num\">Activity</th></tr></thead><tbody>"
        ]
        for row in rows:
            tags = "".join(f'<span class="tag">{html.escape(tag)}</span>' for tag in row.get("tags") or [])
            lines.append(
                f'<tr><td><a href="{topic_prefix}{row["id"]}.html">{html.escape(row.get("title") or "")}</a>'
                f'{" " + tags if tags else ""}</td>'
                f"<td>{self.category_label(row.get('category_id'), category_prefix)}</td>"
                f'<td class="num">{row.get("mirrored_posts", row.get("posts_count", 0))}</td>'
                f'<td class="num">{_when(row.get("bumped_at") or "")}</td></tr>'
            )
        lines.append("</tbody></table>")
        return "".join(lines)

    def sorted_rows(self, category_ids: set | None = None) -> list[dict]:
        rows = [
            row for key, row in self.store.index.items()
            if key in self.mirrored and (category_ids is None or row.get("category_id") in category_ids)
        ]
        return sorted(rows, key=lambda row: (row.get("bumped_at") or "", row["id"]), reverse=True)

    def render(self, changed_only: bool = False) -> None:
        site = self.store.site
        write_bytes(site / "style.css", STYLE.encode("utf-8"))
        if not changed_only:
            for topic_id in sorted(self.mirrored):
                topic = self.store.topic(topic_id)
                if topic:
                    self.render_topic(topic)
        self.render_index()
        children: dict[int, set[int]] = {}
        for category in self.categories.values():
            children.setdefault(category["id"], {category["id"]})
            if category.get("parent_category_id"):
                children.setdefault(category["parent_category_id"], {category["parent_category_id"]}).add(category["id"])
        for category_id in self.categories:
            self.render_category(category_id, children.get(category_id, {category_id}))
        write_bytes(self.store.root / "README.md", self.readme().encode("utf-8"))

    def render_index(self) -> None:
        counts: dict[int, int] = {}
        for row in self.sorted_rows():
            counts[row.get("category_id")] = counts.get(row.get("category_id"), 0) + 1
        items = []
        for category in self.categories.values():
            if category.get("parent_category_id"):
                continue
            subs = [item for item in self.categories.values() if item.get("parent_category_id") == category["id"]]
            total = counts.get(category["id"], 0) + sum(counts.get(item["id"], 0) for item in subs)
            sub_links = ", ".join(self.category_label(item["id"], "c/") for item in subs)
            items.append(
                f"<li>{self.category_label(category['id'], 'c/')} <span class=\"muted\">{total} topics</span>"
                f"{'<br><span class=\"meta\">' + sub_links + '</span>' if sub_links else ''}"
                f"{'<br><span class=\"meta\">' + html.escape(category['description']) + '</span>' if category['description'] else ''}</li>"
            )
        description = html.escape(str(self.store.meta.get("description") or ""))
        body = (
            f'<header class="page"><h1>{html.escape(self.title)}</h1>'
            f'{"<p class=\"muted\">" + description + "</p>" if description else ""}{self.notice()}</header>'
            f"<h2>Categories</h2><ul>{''.join(items)}</ul>"
            f"<h2>Latest</h2>{self.topic_rows(self.sorted_rows()[:LATEST_ON_INDEX], 't/', 'c/')}"
        )
        write_bytes(self.store.site / "index.html", _page(self.title, "style.css", body, "index.html").encode("utf-8"))

    def render_category(self, category_id: int, members: set[int]) -> None:
        category = self.categories[category_id]
        rows = self.sorted_rows(members)
        pages = max(1, math.ceil(len(rows) / TOPICS_PER_PAGE))
        directory = self.store.site / "c"
        for number in range(1, pages + 1):
            chunk = rows[(number - 1) * TOPICS_PER_PAGE:number * TOPICS_PER_PAGE]
            links = "".join(
                f"<span>{index}</span>" if index == number else f'<a href="{_category_file(category_id, index)}">{index}</a>'
                for index in range(1, pages + 1)
            ) if pages > 1 else ""
            parent = self.category_label(category.get("parent_category_id"), "")
            body = (
                f'<header class="page"><p class="meta"><a href="../index.html">{html.escape(self.title)}</a>'
                f'{" / " + parent if parent else ""}</p><h1>{html.escape(category["name"])}</h1>'
                f'{"<p class=\"muted\">" + html.escape(category["description"]) + "</p>" if category["description"] else ""}</header>'
                f"{self.topic_rows(chunk, '../t/', '')}"
                f'{"<nav class=\"pages\">" + links + "</nav>" if links else ""}'
            )
            write_bytes(
                directory / _category_file(category_id, number),
                _page(category["name"], "../style.css", body, "../index.html").encode("utf-8"),
            )
        for stale in directory.glob(f"{category_id}-p*.html"):
            match = re.fullmatch(rf"{category_id}-p(\d+)\.html", stale.name)
            if match and int(match.group(1)) > pages:
                remove_file(stale)

    def render_topic(self, topic: dict) -> None:
        self.mirrored.add(topic["id"])
        posts = []
        for post in topic.get("posts") or []:
            who = html.escape(post["username"])
            if post["name"] and post["name"] != post["username"]:
                who += f' <span class="muted">{html.escape(post["name"])}</span>'
            when = f'<time datetime="{html.escape(post["created_at"], quote=True)}">{_when(post["created_at"])}</time>'
            if post["post_type"] == 3:
                action = html.escape(post["action_code"].replace("_", " ") or "updated the topic")
                posts.append(f'<article class="post small" id="p{post["post_number"]}">{who} {action}, {when}</article>')
                continue
            reply = ""
            if post["reply_to_post_number"]:
                number = post["reply_to_post_number"]
                reply = f' · replying to <a href="#p{number}">#{number}</a>'
            edited = ""
            if post["updated_at"][:16] != post["created_at"][:16] and post["updated_at"]:
                edited = f" · edited {_when(post['updated_at'])}"
            body = sanitize(post["cooked"], self.origin, self.store.uploads, self.mirrored)
            posts.append(
                f'<article class="post" id="p{post["post_number"]}"><p class="meta"><strong>{who}</strong> · '
                f'<a href="#p{post["post_number"]}">#{post["post_number"]}</a> · {when}{reply}{edited}</p>'
                f'<div class="body">{body}</div></article>'
            )
        tags = "".join(f'<span class="tag">{html.escape(tag)}</span>' for tag in topic.get("tags") or [])
        state = " · closed" if topic.get("closed") else ""
        state += " · archived" if topic.get("archived") else ""
        category = self.category_label(topic.get("category_id"), "../c/")
        source = html.escape(topic.get("source") or self.origin, quote=True)
        body = (
            f'<header class="page"><p class="meta"><a href="../index.html">{html.escape(self.title)}</a>'
            f'{" / " + category if category else ""}</p><h1>{html.escape(topic.get("title") or "")}</h1>'
            f'<p class="meta">{tags}{len(posts)} posts{state} · '
            f'<a href="{source}" rel="nofollow noopener noreferrer">original</a></p></header>'
            + "".join(posts)
        )
        write_bytes(
            self.store.site / "t" / f"{topic['id']}.html",
            _page(topic.get("title") or self.title, "../style.css", body, "../index.html").encode("utf-8"),
        )

    def readme(self) -> str:
        meta = self.store.meta
        return (
            f"# {self.title}\n\n"
            f"A read-only copy of the Discourse forum at <{self.origin}>, made by the ForkMesh\n"
            f"Discourse Mirror plugin. Last copied {meta.get('lastSync', '')}: {meta.get('topics', 0)} topics,\n"
            f"{meta.get('posts', 0)} posts.\n\n"
            "Open `site/index.html` in a browser to read it. It loads nothing from the\n"
            "network: pictures that were copied live in `uploads/`, and the rest link to\n"
            "the forum. `topics/<id>.json` holds each topic's posts as the forum served\n"
            "them, `categories.json` the categories and `mirror.json` the copy's details.\n\n"
            "Posts belong to their authors under the source forum's terms. To reply,\n"
            "like or report a post, use the forum itself.\n"
        )


def _category_file(category_id: int, number: int) -> str:
    return f"{category_id}.html" if number == 1 else f"{category_id}-p{number}.html"


def git(repository: Path, *args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["git", "-C", str(repository), *args], capture_output=True, text=True, check=False
    )


def ensure_repository(repository: Path, branch: str, create: bool = True) -> None:
    if (repository / ".git").exists():
        return
    if not create:
        raise MirrorError(f"{repository} is not a Git checkout; point --repository at the ForkMesh repository's checkout")
    repository.mkdir(parents=True, exist_ok=True)
    result = git(repository, "init", "-q", "-b", branch)
    if result.returncode:
        raise MirrorError(f"git init failed in {repository}: {result.stderr.strip()}")


def commit_repository(forum: dict, message: str) -> str:
    repository = Path(forum["repository"])
    scope = FORUM_PATH if forum.get("layout") == "forum" else "."
    result = git(repository, "add", "-A", "--", scope)
    if result.returncode:
        raise MirrorError(f"git add failed: {result.stderr.strip()}")
    if not git(repository, "diff", "--cached", "--quiet", "--", scope).returncode:
        return ""
    result = git(
        repository, "-c", f"user.name={forum['commitName']}", "-c", f"user.email={forum['commitEmail']}",
        "commit", "-q", "--no-verify", "-m", message, "--", scope,
    )
    if result.returncode:
        raise MirrorError(f"git commit failed: {result.stderr.strip()}")
    return git(repository, "rev-parse", "HEAD").stdout.strip()


def push_repository(forum: dict) -> None:
    remote = str(forum.get("pushRemote") or "")
    if not remote:
        return
    result = git(Path(forum["repository"]), "push", "-q", remote, f"HEAD:refs/heads/{forum['branch']}")
    if result.returncode:
        raise MirrorError(
            f"git push to {remote} failed; the copy is committed and was left as it is: {result.stderr.strip()}"
        )


@contextlib.contextmanager
def forum_lock(data_dir: Path, name: str):
    path = data_dir / "locks" / f"{name}.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a+") as handle:
        try:
            fcntl.flock(handle, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise MirrorError(f"{name} is already being copied") from error
        yield


def check_forum(origin: str, sleep=time.sleep, delay: float = 1.0) -> dict:
    forum = {**DEFAULTS, "origin": normalize_origin(origin), "delaySeconds": delay}
    client = Client(forum, sleep=sleep)
    report = {"origin": forum["origin"], "discourse": False, "public": False, "robotsAllow": False}
    report["robotsAllow"] = all(
        client.robots_allow(client.url(path)) for path in ("/site.json", "/latest.json", "/t/1.json")
    )
    status, _, body = client._open(client.url("/site.json"), "application/json", MAX_JSON_BYTES, {client.host})
    report["public"] = status == 200
    site = {}
    with contextlib.suppress(ValueError):
        site = json.loads(body) if status == 200 else {}
    site = site if isinstance(site, dict) else {}
    report["discourse"] = isinstance(site.get("categories"), list)
    report["categories"] = len(site.get("categories") or [])
    about = {}
    if report["discourse"] and report["public"]:
        with contextlib.suppress(MirrorError):
            about = (client.json("/about.json", missing_ok=True) or {}).get("about") or {}
    stats = about.get("stats") or about
    topics = _int(stats.get("topics_count", stats.get("topic_count")))
    posts = _int(stats.get("posts_count", stats.get("post_count")))
    report.update(title=str(about.get("title") or ""), topics=topics, posts=posts)
    requests = math.ceil(topics / 30) + topics + max(0, posts - POSTS_PER_REQUEST * topics) // POSTS_PER_REQUEST
    report["firstCopyRequests"] = requests
    report["firstCopyHours"] = round(requests * DEFAULTS["delaySeconds"] / 3600, 1)
    report["ready"] = report["discourse"] and report["public"] and report["robotsAllow"]
    return report


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        prog="discourse_mirror.py",
        description="Copy public Discourse forums into Git repositories that ForkMesh mirrors carry.",
    )
    parser.add_argument("--data-dir", help="plugin data folder (default: the project's data/discourse-mirror)")
    commands = parser.add_subparsers(dest="command", required=True)
    check = commands.add_parser("check", help="check that a forum can be copied, without saving anything")
    check.add_argument("forum", help="forum address or a configured name")
    add = commands.add_parser("add", help="add a forum to copy")
    add.add_argument("origin", help="forum address, such as https://forum.example.org or https://example.org/forum")
    add.add_argument("--name", required=True, help="short name: lowercase letters, digits and dashes")
    add.add_argument("--repository", help="Git checkout to write (default: data/discourse-mirror/forums/<name>)")
    add.add_argument("--push-remote", default="", help="Git remote to push after each copy")
    add.add_argument("--no-uploads", action="store_true", help="do not copy pictures")
    add.add_argument("--forum", action="store_true",
                     help=f"write the forum into {FORUM_PATH} of an existing ForkMesh repository checkout")
    add.add_argument("--delay", type=float, default=DEFAULTS["delaySeconds"], help="seconds between requests")
    commands.add_parser("list", help="list the configured forums")
    remove = commands.add_parser("remove", help="stop copying a forum; its repository is kept")
    remove.add_argument("name")
    sync = commands.add_parser("sync", help="copy what changed since the last run")
    sync.add_argument("names", nargs="*", help="forums to copy (default: all)")
    sync.add_argument("--full", action="store_true", help="walk the whole topic list, not only the newest pages")
    sync.add_argument("--max-topics", type=int, help="fetch at most this many topics in this run")
    sync.add_argument("--no-push", action="store_true", help="commit but do not push")
    render = commands.add_parser("render", help="rebuild the HTML pages from the saved topics")
    render.add_argument("name")
    ci = commands.add_parser(
        "ci", help=f"copy into the repository this runs in, using {CI_SETTINGS} (for a scheduled workflow)")
    ci.add_argument("--workspace", help="repository checkout (default: $GITHUB_WORKSPACE or the current folder)")
    ci.add_argument("--max-minutes", type=float, default=45, help="stop fetching after this long and commit")
    fold = commands.add_parser("fold", help="rebuild a repository forum's list files without copying anything")
    fold.add_argument("--workspace", help="repository checkout (default: $GITHUB_WORKSPACE or the current folder)")
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(sys.argv[1:] if argv is None else argv)
    if args.command == "ci":
        return run_ci(args)
    if args.command == "fold":
        return run_fold(args)
    try:
        data_dir = Path(args.data_dir).expanduser() if args.data_dir else default_data_dir()
        if args.command == "check":
            config = Config(data_dir) if (data_dir / "forums.json").exists() else None
            origin = args.forum
            if config and NAME_RE.fullmatch(args.forum):
                with contextlib.suppress(MirrorError):
                    origin = config.find(args.forum)["origin"]
            report = check_forum(origin)
            print(json.dumps(report, indent=1))
            return 0 if report["ready"] else 2
        config = Config(data_dir)
        if args.command == "add":
            if not NAME_RE.fullmatch(args.name):
                raise MirrorError("the name must be lowercase letters, digits and dashes")
            if any(forum.get("name") == args.name for forum in config.forums):
                raise MirrorError(f"{args.name} is already configured")
            entry = {"name": args.name, "origin": normalize_origin(args.origin)}
            if args.repository:
                entry["repository"] = str(Path(args.repository).expanduser().resolve())
            if args.push_remote:
                entry["pushRemote"] = args.push_remote
            if args.no_uploads:
                entry["uploads"] = False
            if args.forum:
                if not args.repository:
                    raise MirrorError("--forum needs --repository, the ForkMesh repository's checkout")
                entry["layout"] = "forum"
            if args.delay != DEFAULTS["delaySeconds"]:
                entry["delaySeconds"] = max(0.5, args.delay)
            config.forums.append(entry)
            config.save()
            print(json.dumps(config.resolved(entry), indent=1))
            return 0
        if args.command == "list":
            print(json.dumps([config.resolved(forum) for forum in config.forums], indent=1))
            return 0
        if args.command == "remove":
            forum = config.find(args.name)
            config.forums = [item for item in config.forums if item.get("name") != args.name]
            config.save()
            print(f"{args.name} is no longer copied; its repository stays at {forum['repository']}")
            return 0
        if args.command == "render":
            forum = config.find(args.name)
            with forum_lock(data_dir, forum["name"]):
                if forum["layout"] == "forum":
                    ForumStore(Path(forum["repository"]) / FORUM_PATH, forum["origin"]).save()
                else:
                    Renderer(Store(Path(forum["repository"]))).render()
                print(commit_repository(forum, "Mirror: rebuild pages") or "no changes")
            return 0
        names = args.names or [forum["name"] for forum in config.forums]
        failed = False
        for name in names:
            forum = config.find(name)
            with forum_lock(data_dir, forum["name"]):
                result = Mirror(forum).sync(full=args.full, max_topics=args.max_topics)
                if not args.no_push:
                    try:
                        push_repository(forum)
                    except MirrorError as error:
                        result["error"] = str(error)
            print(json.dumps({"forum": name, **result}))
            failed = failed or bool(result["error"])
        return 1 if failed else 0
    except MirrorError as error:
        print(f"discourse_mirror: {error}", file=sys.stderr)
        return 1


def ci_forum(workspace: Path) -> dict:
    settings = read_json(workspace / CI_SETTINGS, None)
    if not isinstance(settings, dict) or not settings.get("origin"):
        raise MirrorError(f"{workspace / CI_SETTINGS} does not name a forum")
    forum = {key: settings[key] for key in CI_KEYS if key in settings}
    forum.update(name="ci", origin=normalize_origin(settings["origin"]), repository=str(workspace),
                 layout="forum", maxTopicsPerRun=int(settings.get("maxTopicsPerRun") or 400))
    forum["delaySeconds"] = max(1.0, float(forum.get("delaySeconds") or 1.0))
    return {**DEFAULTS, **forum}


def run_fold(args: argparse.Namespace) -> int:
    workspace = Path(args.workspace or os.environ.get("GITHUB_WORKSPACE") or ".").resolve()
    root = workspace / FORUM_PATH
    forum = read_json(root / "forum.json", None)
    if not isinstance(forum, dict):
        print(f"discourse_mirror: {root / 'forum.json'} is missing", file=sys.stderr)
        return 1
    source = forum.get("source") if isinstance(forum.get("source"), dict) else {}
    store = ForumStore(root, str(source.get("origin") or ""))
    store.meta.update(lastSync=source.get("lastSync", ""), pending=source.get("pending", 0))
    store.save()
    entry = {**DEFAULTS, "repository": str(workspace), "layout": "forum"}
    try:
        commit = commit_repository(entry, "Forum: rebuild the lists")
    except MirrorError as error:
        print(f"discourse_mirror: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"commit": commit}))
    return 0


def run_ci(args: argparse.Namespace) -> int:
    workspace = Path(args.workspace or os.environ.get("GITHUB_WORKSPACE") or ".").resolve()
    try:
        forum = ci_forum(workspace)
        mirror = Mirror(forum)
        mirror.deadline = time.monotonic() + max(1.0, args.max_minutes) * 60
        result = mirror.sync()
    except MirrorError as error:
        print(f"discourse_mirror: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"forum": forum["origin"], **result}))
    return 1 if result["error"] else 0


if __name__ == "__main__":
    sys.exit(main())
